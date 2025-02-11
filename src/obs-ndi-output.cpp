/*
obs-ndi
Copyright (C) 2016-2018 Stéphane Lepin <steph  name of author

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/profiler.h>
#include <util/circlebuf.h>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QLibrary>
#include <QMainWindow>
#include <QAction>
#include <QMessageBox>
#include <QString>
#include <QStringList>

#include "obs-ndi.h"

const NDIlib_v4* load_ndilib_output(void*);

static FORCE_INLINE uint32_t min_uint32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

typedef void (*uyvy_conv_function)(uint8_t* input[], uint32_t in_linesize[],
							  uint32_t start_y, uint32_t end_y,
							  uint8_t* output, uint32_t out_linesize);

static void convert_i444_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
						  uint32_t start_y, uint32_t end_y,
						  uint8_t* output, uint32_t out_linesize)
{
	uint8_t* _Y;
	uint8_t* _U;
	uint8_t* _V;
	uint8_t* _out;
	uint32_t width = min_uint32(in_linesize[0], out_linesize);
	for (uint32_t y = start_y; y < end_y; ++y) {
		_Y = input[0] + ((size_t)y * (size_t)in_linesize[0]);
		_U = input[1] + ((size_t)y * (size_t)in_linesize[1]);
		_V = input[2] + ((size_t)y * (size_t)in_linesize[2]);

		_out = output + ((size_t)y * (size_t)out_linesize);

		for (uint32_t x = 0; x < width; x += 2) {
			// Quality loss here. Some chroma samples are ignored.
			*(_out++) = *(_U++); _U++;
			*(_out++) = *(_Y++);
			*(_out++) = *(_V++); _V++;
			*(_out++) = *(_Y++);
		}
	}
}

struct ndi_output
{
	obs_output_t *output;
	const char* ndi_name;
	
	bool uses_video;
	bool uses_audio;

	bool started;
	NDIlib_send_instance_t ndi_sender;

	uint32_t frame_width;
	uint32_t frame_height;
	NDIlib_FourCC_video_type_e frame_fourcc;
	double video_framerate;

	size_t audio_channels;
	uint32_t audio_samplerate;

	uint8_t* conv_buffer;
	uint32_t conv_linesize;
	uyvy_conv_function conv_function;

	uint8_t* audio_conv_buffer;
	size_t audio_conv_buffer_size;

	os_performance_token_t* perf_token;
	
	bool synthesise_video_timestamps;
	bool synthesise_audio_timestamps;
	bool async_video_send;
	
	const NDIlib_v4* ndiLib;
	QLibrary* loaded_lib;
};

const char* ndi_output_getname(void* data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("NDIPlugin.OutputName");
}

obs_properties_t* ndi_output_getproperties(void* data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t* props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "ndi_name",
		obs_module_text("NDIPlugin.OutputProps.NDIName"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, SYNTHESISE_VIDEO_TIMESTAMPS,
		"Synthesise Video Timecode");
	obs_properties_add_bool(props, SYNTHESISE_AUDIO_TIMESTAMPS,
		"Synthesise Audio Timecode");

	return props;
}

void ndi_output_getdefaults(obs_data_t* settings)
{
	obs_data_set_default_string(settings,
								"ndi_name", "obs-ndi output (changeme)");
	obs_data_set_default_bool(settings, "uses_video", true);
	obs_data_set_default_bool(settings, "uses_audio", true);
	
	obs_data_set_bool(settings, SYNTHESISE_VIDEO_TIMESTAMPS, true);
	obs_data_set_bool(settings, SYNTHESISE_AUDIO_TIMESTAMPS, true);
	obs_data_set_bool(settings, ASYNC_VIDEO_SEND, true);
}

bool ndi_output_start(void* data)
{
	auto o = (struct ndi_output*)data;

	uint32_t flags = 0;
	video_t* video = obs_output_video(o->output);
	audio_t* audio = obs_output_audio(o->output);

	if (!video && !audio) {
		blog(LOG_ERROR, "'%s': no video and audio available", o->ndi_name);
		return false;
	}

	if (o->synthesise_video_timestamps) {
		blog(LOG_INFO, "'%s': synthesise video timestamps", o->ndi_name);
	}
	if (o->synthesise_audio_timestamps) {
		blog(LOG_INFO, "'%s': synthesise audio timestamps", o->ndi_name);
	}
	if (o->async_video_send) {
		blog(LOG_INFO, "'%s': async video send", o->ndi_name);
	}
	
	if (o->uses_video && video) {
		video_format format = video_output_get_format(video);
		uint32_t width = video_output_get_width(video);
		uint32_t height = video_output_get_height(video);

		switch (format) {
			case VIDEO_FORMAT_I444:
				o->conv_function = convert_i444_to_uyvy;
				o->frame_fourcc = NDIlib_FourCC_video_type_UYVY;
				o->conv_linesize = width * 2;
				o->conv_buffer = new uint8_t[(size_t)height * (size_t)o->conv_linesize * 2]();
				break;

			case VIDEO_FORMAT_NV12:
				o->frame_fourcc = NDIlib_FourCC_video_type_NV12;
				break;

			case VIDEO_FORMAT_I420:
				o->frame_fourcc = NDIlib_FourCC_video_type_I420;
				break;

			case VIDEO_FORMAT_RGBA:
				o->frame_fourcc = NDIlib_FourCC_video_type_RGBA;
				break;

			case VIDEO_FORMAT_BGRA:
				o->frame_fourcc = NDIlib_FourCC_video_type_BGRA;
				break;

			case VIDEO_FORMAT_BGRX:
				o->frame_fourcc = NDIlib_FourCC_video_type_BGRX;
				break;

			default:
				blog(LOG_WARNING, "unsupported pixel format %d", format);
				return false;
		}

		o->frame_width = width;
		o->frame_height = height;
		o->video_framerate = video_output_get_frame_rate(video);
		flags |= OBS_OUTPUT_VIDEO;
	}

	if (o->uses_audio && audio) {
		o->audio_samplerate = audio_output_get_sample_rate(audio);
		o->audio_channels = audio_output_get_channels(audio);
		flags |= OBS_OUTPUT_AUDIO;
	}

	NDIlib_send_create_t send_desc;
	send_desc.p_ndi_name = o->ndi_name;
	send_desc.p_groups = o->ndi_name;
	send_desc.clock_video = false;
	send_desc.clock_audio = false;

	o->ndi_sender = o->ndiLib->send_create(&send_desc);
	if (o->ndi_sender) {
		if (o->perf_token) {
			os_end_high_performance(o->perf_token);
		}
		o->perf_token = os_request_high_performance("NDI Output");

		o->started = obs_output_begin_data_capture(o->output, flags);
		if (o->started) {
			blog(LOG_INFO, "'%s': ndi output started", o->ndi_name);
		} else {
			blog(LOG_ERROR, "'%s': data capture start failed", o->ndi_name);
		}
	} else {
		blog(LOG_ERROR, "'%s': ndi sender init failed", o->ndi_name);
	}

	return o->started;
}

void ndi_output_stop(void* data, uint64_t ts)
{
	auto o = (struct ndi_output*)data;

	o->started = false;
	obs_output_end_data_capture(o->output);

	os_end_high_performance(o->perf_token);
	o->perf_token = NULL;

	o->ndiLib->send_destroy(o->ndi_sender);
	if (o->conv_buffer) {
		delete o->conv_buffer;
		o->conv_function = nullptr;
	}

	o->frame_width = 0;
	o->frame_height = 0;
	o->video_framerate = 0.0;

	o->audio_channels = 0;
	o->audio_samplerate = 0;
}

void ndi_output_update(void* data, obs_data_t* settings)
{
	auto o = (struct ndi_output*)data;
	o->ndi_name = obs_data_get_string(settings, "ndi_name");
	o->uses_video = obs_data_get_bool(settings, "uses_video");
	o->uses_audio = obs_data_get_bool(settings, "uses_audio");
	o->synthesise_video_timestamps = obs_data_get_bool(settings, SYNTHESISE_VIDEO_TIMESTAMPS);
	o->synthesise_audio_timestamps = obs_data_get_bool(settings, SYNTHESISE_AUDIO_TIMESTAMPS);
	o->async_video_send = obs_data_get_bool(settings, ASYNC_VIDEO_SEND);
}

void* ndi_output_create(obs_data_t* settings, obs_output_t* output)
{
	auto o = (struct ndi_output*)bzalloc(sizeof(struct ndi_output));
	o->ndiLib = load_ndilib_output(o);
	o->output = output;
	o->started = false;
	o->audio_conv_buffer = nullptr;
	o->audio_conv_buffer_size = 0;
	o->perf_token = NULL;
	ndi_output_update(o, settings);
	return o;
}

void ndi_output_destroy(void* data)
{
	auto o = (struct ndi_output*)data;
	if (o->audio_conv_buffer) {
		bfree(o->audio_conv_buffer);
	}
	
	if (o->ndiLib) {
		o->ndiLib->destroy();
	}

	if (o->loaded_lib) {
		delete o->loaded_lib;
	}
	bfree(o);
}

int inline get_frame_rate_N (double fr) 
{
	int fr_i = int(fr*100.0f); // only works for progressive (from NDI SDK docs)
	
	switch (fr_i) {
		case 5994:
			return 60000;
		break;
		case 5000:
			return 30000;
		break;
		case 2500:
			return 30000;
		break;
		case 3000:
			return 30000;
		break;
		case 2400:
			return 24000;
		break;
		default:
			return 30000;
		break;
	
	}
}

int inline get_frame_rate_D (double fr) 
{
	int fr_i = int(fr*100.0f); // only works for progressive (from NDI SDK docs)
	
	switch (fr_i) {
		case 5994:
			return 1001;
		break;
		case 5000:
			return 1200;
		break;
		case 25000:
			return 1200;
		break;
		case 3000:
			return 1001;
		break;
		case 2400:
			return 1001;
		break;
		default:
			return 1001;
		break;
	
	}
}

void ndi_output_rawvideo(void* data, struct video_data* frame)
{
	auto o = (struct ndi_output*)data;

	if (!o->started || !o->frame_width || !o->frame_height)
		return;

	uint32_t width = o->frame_width;
	uint32_t height = o->frame_height;

	NDIlib_video_frame_v2_t video_frame = {0};
	video_frame.xres = width;
	video_frame.yres = height;
	video_frame.frame_rate_N = (int)(o->video_framerate * 100);
	video_frame.frame_rate_D = 100; // TODO fixme: broken on fractional framerates
//	video_frame.frame_rate_N = get_frame_rate_N(o->video_framerate);
//	video_frame.frame_rate_D = get_frame_rate_D(o->video_framerate);
	video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
	
	if (o->synthesise_video_timestamps) {
		video_frame.timecode = NDIlib_send_timecode_synthesize;
	} else {
		video_frame.timecode = (int64_t)(obs_get_video_frame_time() / 100);
	}
	video_frame.FourCC = o->frame_fourcc;

	if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
		o->conv_function(frame->data, frame->linesize,
							 0, height,
							 o->conv_buffer, o->conv_linesize);
		video_frame.p_data = o->conv_buffer;
		video_frame.line_stride_in_bytes = o->conv_linesize;
	}
	else {
		video_frame.p_data = frame->data[0];
		video_frame.line_stride_in_bytes = frame->linesize[0];
	}

	if (o->async_video_send) {
		o->ndiLib->send_send_video_async_v2(o->ndi_sender, &video_frame);
	} else {
		o->ndiLib->send_send_video_v2(o->ndi_sender, &video_frame);
	}
}

void ndi_output_rawaudio(void* data, struct audio_data* frame)
{
	auto o = (struct ndi_output*)data;

	if (!o->started || !o->audio_samplerate || !o->audio_channels)
		return;

	NDIlib_audio_frame_v3_t audio_frame = {0};
	audio_frame.sample_rate = o->audio_samplerate;
	audio_frame.no_channels = (int)o->audio_channels;
	audio_frame.no_samples = frame->frames;
	audio_frame.channel_stride_in_bytes = frame->frames * 4;
	audio_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;

	const size_t data_size =
		(size_t)audio_frame.no_channels * (size_t)audio_frame.channel_stride_in_bytes;
	if (data_size > o->audio_conv_buffer_size) {
		if (o->audio_conv_buffer) {
			bfree(o->audio_conv_buffer);
		}
		o->audio_conv_buffer = (uint8_t*)bmalloc(data_size);
		o->audio_conv_buffer_size = data_size;
	}

	for (int i = 0; i < audio_frame.no_channels; ++i) {
		memcpy(o->audio_conv_buffer + ((size_t)i * (size_t)audio_frame.channel_stride_in_bytes),
			frame->data[i],
			audio_frame.channel_stride_in_bytes);
	}

	audio_frame.p_data = o->audio_conv_buffer;

	if (o->synthesise_audio_timestamps) {
		audio_frame.timecode = NDIlib_send_timecode_synthesize;
	} else {
		audio_frame.timecode = (int64_t)(frame->timestamp / 100);
	}

	o->ndiLib->send_send_audio_v3(o->ndi_sender, &audio_frame);
}

struct obs_output_info create_ndi_output_info()
{
	struct obs_output_info ndi_output_info = {};
	ndi_output_info.id				= "ndi_output";
	ndi_output_info.flags			= OBS_OUTPUT_AV;
	ndi_output_info.get_name		= ndi_output_getname;
	ndi_output_info.get_properties	= ndi_output_getproperties;
	ndi_output_info.get_defaults	= ndi_output_getdefaults;
	ndi_output_info.create			= ndi_output_create;
	ndi_output_info.destroy			= ndi_output_destroy;
	ndi_output_info.update			= ndi_output_update;
	ndi_output_info.start			= ndi_output_start;
	ndi_output_info.stop			= ndi_output_stop;
	ndi_output_info.raw_video		= ndi_output_rawvideo;
	ndi_output_info.raw_audio		= ndi_output_rawaudio;

	return ndi_output_info;
}

typedef const NDIlib_v4* (*NDIlib_v4_load_)(void);

const NDIlib_v4* load_ndilib_output(void* data)
{
	auto s = (struct ndi_output*)data;
	QStringList locations;
	locations << QString(qgetenv(NDILIB_REDIST_FOLDER));
#if defined(__linux__) || defined(__APPLE__)
	locations << "/usr/lib";
	locations << "/usr/local/lib";
#endif

	for (QString path : locations) {
		blog(LOG_INFO, "Trying '%s'", path.toUtf8().constData());
		QFileInfo libPath(QDir(path).absoluteFilePath(NDILIB_LIBRARY_NAME));

		if (libPath.exists() && libPath.isFile()) {
			QString libFilePath = libPath.absoluteFilePath();
			blog(LOG_INFO, "Found NDI library at '%s'",
				libFilePath.toUtf8().constData());

			s->loaded_lib = new QLibrary(libFilePath, nullptr);
			if (s->loaded_lib->load()) {
				blog(LOG_INFO, "NDI runtime loaded successfully");

				NDIlib_v4_load_ lib_load =
					(NDIlib_v4_load_)s->loaded_lib->resolve("NDIlib_v4_load");

				if (lib_load != nullptr) {
					return lib_load();
				}
				else {
					blog(LOG_INFO, "ERROR: NDIlib_v4_load not found in loaded library");
				}
			}
			else {
				delete s->loaded_lib;
				s->loaded_lib = nullptr;
			}
		}
	}

	blog(LOG_ERROR, "Can't find the NDI library");
	return nullptr;
}
