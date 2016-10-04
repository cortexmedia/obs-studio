/******************************************************************************
    Copyright (C) 2016 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <obs-avc.h>

#include <libavutil/opt.h>
#include <libavformat/avformat.h>

#include "obs-ffmpeg-formats.h"

#define do_log(level, format, ...) \
	blog(level, "[NVENC encoder: '%s'] " format, \
			obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

struct nvenc_encoder {
	obs_encoder_t                  *encoder;

	AVCodec                        *nvenc;
	AVCodecContext                 *context;

	AVPicture                      dst_picture;
	AVFrame                        *vframe;

	DARRAY(uint8_t)                buffer;

	uint8_t                        *header;
	size_t                         header_size;

	uint8_t                        *sei;
	size_t                         sei_size;

	int                            height;
	bool                           first_packet;
	bool                           initialized;
};

static const char *nvenc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "NVENC H.264";
}

static inline bool valid_format(enum video_format format)
{
	return format == VIDEO_FORMAT_I420 ||
	       format == VIDEO_FORMAT_NV12 ||
	       format == VIDEO_FORMAT_I444;
}

static void nvenc_video_info(void *data, struct video_scale_info *info)
{
	struct nvenc_encoder *enc = data;
	enum video_format pref_format;

	pref_format = obs_encoder_get_preferred_video_format(enc->encoder);

	if (!valid_format(pref_format)) {
		pref_format = valid_format(info->format) ?
			info->format : VIDEO_FORMAT_NV12;
	}

	info->format = pref_format;
}

static bool nvenc_init_codec(struct nvenc_encoder *enc)
{
	int ret;

	ret = avcodec_open2(enc->context, enc->nvenc, NULL);
	if (ret < 0) {
		warn("Failed to open NVENC codec: %s", av_err2str(ret));
		return false;
	}

	enc->vframe = av_frame_alloc();
	if (!enc->vframe) {
		warn("Failed to allocate video frame");
		return false;
	}

	enc->vframe->format = enc->context->pix_fmt;
	enc->vframe->width = enc->context->width;
	enc->vframe->height = enc->context->height;
	enc->vframe->colorspace = enc->context->colorspace;
	enc->vframe->color_range = enc->context->color_range;

	ret = avpicture_alloc(&enc->dst_picture, enc->context->pix_fmt,
			enc->context->width, enc->context->height);
	if (ret < 0) {
		warn("Failed to allocate dst_picture: %s", av_err2str(ret));
		return false;
	}

	enc->initialized = true;

	*((AVPicture*)enc->vframe) = enc->dst_picture;
	return true;
}

enum RC_MODE {
	RC_MODE_CBR,
	RC_MODE_VBR,
	RC_MODE_CQP,
	RC_MODE_LOSSLESS
};

static bool nvenc_update(void *data, obs_data_t *settings)
{
	struct nvenc_encoder *enc = data;

	const char *rc = obs_data_get_string(settings, "rate_control");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	int cqp = (int)obs_data_get_int(settings, "cqp");
	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	const char *level = obs_data_get_string(settings, "level");
	bool twopass = obs_data_get_bool(settings, "2pass");
	int gpu = (int)obs_data_get_int(settings, "gpu");
	bool cbr_override = obs_data_get_bool(settings, "cbr");
	int bf = (int)obs_data_get_int(settings, "bf");

	video_t *video = obs_encoder_video(enc->encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	struct video_scale_info info;

	/* XXX: "cbr" setting has been deprecated */
	if (cbr_override) {
		warn("\"cbr\" setting has been deprecated for all encoders!  "
		     "Please set \"rate_control\" to \"CBR\" instead.  "
		     "Forcing CBR mode.  "
		     "(Note to all: this is why you shouldn't use strings for "
		     "common settings)");
		rc = "CBR";
	}

	info.format = voi->format;
	info.colorspace = voi->colorspace;
	info.range = voi->range;

	nvenc_video_info(enc, &info);
	av_opt_set_int(enc->context->priv_data, "cbr", false, 0);
	av_opt_set(enc->context->priv_data, "profile", profile, 0);
	av_opt_set(enc->context->priv_data, "preset", preset, 0);

	if (astrcmpi(rc, "cqp") == 0) {
		bitrate = 0;
		enc->context->global_quality = cqp;

	} else if (astrcmpi(rc, "lossless") == 0) {
		bitrate = 0;
		cqp = 0;

		bool hp = (astrcmpi(preset, "hp") == 0 ||
		           astrcmpi(preset, "llhp") == 0);

		av_opt_set(enc->context->priv_data, "preset",
				hp ? "losslesshp" : "lossless", 0);

	} else if (astrcmpi(rc, "vbr") != 0) { /* CBR by default */
		av_opt_set_int(enc->context->priv_data, "cbr", true, 0);
		enc->context->rc_max_rate = bitrate * 1000;
		enc->context->rc_min_rate = bitrate * 1000;
		cqp = 0;
	}


	av_opt_set(enc->context->priv_data, "level", level, 0);
	av_opt_set_int(enc->context->priv_data, "2pass", twopass, 0);
	av_opt_set_int(enc->context->priv_data, "gpu", gpu, 0);

	enc->context->bit_rate = bitrate * 1000;
	enc->context->rc_buffer_size = bitrate * 1000;
	enc->context->width = obs_encoder_get_width(enc->encoder);
	enc->context->height = obs_encoder_get_height(enc->encoder);
	enc->context->time_base = (AVRational){voi->fps_den, voi->fps_num};
	enc->context->pix_fmt = obs_to_ffmpeg_video_format(info.format);
	enc->context->colorspace = info.colorspace == VIDEO_CS_709 ?
		AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
	enc->context->color_range = info.range == VIDEO_RANGE_FULL ?
		AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	enc->context->max_b_frames = bf;

	if (keyint_sec)
		enc->context->gop_size = keyint_sec * voi->fps_num /
			voi->fps_den;
	else
		enc->context->gop_size = 250;

	enc->height = enc->context->height;

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\tlevel:        %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\t2-pass:       %s\n"
	     "\tGPU:          %d\n",
	     rc, bitrate, cqp, enc->context->gop_size,
	     preset, profile, level,
	     enc->context->width, enc->context->height,
	     twopass ? "true" : "false",
	     gpu);

	return nvenc_init_codec(enc);
}

static void nvenc_destroy(void *data)
{
	struct nvenc_encoder *enc = data;

	if (enc->initialized) {
		AVPacket pkt = {0};
		int r_pkt = 1;

		while (r_pkt) {
			if (avcodec_encode_video2(enc->context, &pkt, NULL,
						&r_pkt) < 0)
				break;

			if (r_pkt)
				av_free_packet(&pkt);
		}
	}

	avcodec_close(enc->context);
	av_frame_free(&enc->vframe);
	avpicture_free(&enc->dst_picture);
	da_free(enc->buffer);
	bfree(enc->header);
	bfree(enc->sei);

	bfree(enc);
}

static void *nvenc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct nvenc_encoder *enc;

	avcodec_register_all();

	enc = bzalloc(sizeof(*enc));
	enc->encoder = encoder;
	enc->nvenc = avcodec_find_encoder_by_name("nvenc_h264");
	enc->first_packet = true;

	blog(LOG_INFO, "---------------------------------");

	if (!enc->nvenc) {
		warn("Couldn't find encoder");
		goto fail;
	}

	enc->context = avcodec_alloc_context3(enc->nvenc);
	if (!enc->context) {
		warn("Failed to create codec context");
		goto fail;
	}

	if (!nvenc_update(enc, settings))
		goto fail;

	return enc;

fail:
	nvenc_destroy(enc);
	return NULL;
}

static inline void copy_data(AVPicture *pic, const struct encoder_frame *frame,
		int height)
{
	for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane])
			continue;

		int frame_rowsize = (int)frame->linesize[plane];
		int pic_rowsize   = pic->linesize[plane];
		int bytes = frame_rowsize < pic_rowsize ?
			frame_rowsize : pic_rowsize;
		int plane_height = plane == 0 ? height : height/2;

		for (int y = 0; y < plane_height; y++) {
			int pos_frame = y * frame_rowsize;
			int pos_pic   = y * pic_rowsize;

			memcpy(pic->data[plane] + pos_pic,
			       frame->data[plane] + pos_frame,
			       bytes);
		}
	}
}

static bool nvenc_encode(void *data, struct encoder_frame *frame,
		struct encoder_packet *packet, bool *received_packet)
{
	struct nvenc_encoder *enc = data;
	AVPacket av_pkt = {0};
	int got_packet;
	int ret;

	av_init_packet(&av_pkt);

	copy_data(&enc->dst_picture, frame, enc->height);

	enc->vframe->pts = frame->pts;
	ret = avcodec_encode_video2(enc->context, &av_pkt, enc->vframe,
			&got_packet);
	if (ret < 0) {
		warn("nvenc_encode: Error encoding: %s", av_err2str(ret));
		return false;
	}

	if (got_packet && av_pkt.size) {
		if (enc->first_packet) {
			uint8_t *new_packet;
			size_t size;

			enc->first_packet = false;
			obs_extract_avc_headers(av_pkt.data, av_pkt.size,
					&new_packet, &size,
					&enc->header, &enc->header_size,
					&enc->sei, &enc->sei_size);

			da_copy_array(enc->buffer, new_packet, size);
			bfree(new_packet);
		} else {
			da_copy_array(enc->buffer, av_pkt.data, av_pkt.size);
		}

		packet->pts = av_pkt.pts;
		packet->dts = av_pkt.dts;
		packet->data = enc->buffer.array;
		packet->size = enc->buffer.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->keyframe = obs_avc_keyframe(packet->data, packet->size);
		*received_packet = true;
	} else {
		*received_packet = false;
	}

	av_free_packet(&av_pkt);
	return true;
}

static void nvenc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 850);
	obs_data_set_default_int(settings, "keyint_sec", 0);
	obs_data_set_default_int(settings, "cqp", 23);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_string(settings, "preset", "default");
	obs_data_set_default_string(settings, "profile", "main");
	obs_data_set_default_string(settings, "level", "auto");
	obs_data_set_default_bool(settings, "2pass", true);
	obs_data_set_default_int(settings, "gpu", 0);
	obs_data_set_default_int(settings, "bf", 2);
}

static bool rate_control_modified(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");
	bool cqp = astrcmpi(rc, "CQP") == 0;
	bool lossless = astrcmpi(rc, "lossless") == 0;
	size_t count;

	p = obs_properties_get(ppts, "bitrate");
	obs_property_set_visible(p, !cqp && !lossless);
	p = obs_properties_get(ppts, "cqp");
	obs_property_set_visible(p, cqp);

	p = obs_properties_get(ppts, "preset");
	count = obs_property_list_item_count(p);

	for (size_t i = 0; i < count; i++) {
		bool compatible = (i == 0 || i == 2);
		obs_property_list_item_disable(p, i, lossless && !compatible);
	}

	return true;
}

static obs_properties_t *nvenc_properties(void *unused, obs_data_t *settings)
{
	UNUSED_PARAMETER(unused);
	UNUSED_PARAMETER(settings);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "rate_control",
			obs_module_text("RateControl"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "CBR", "CBR");
	obs_property_list_add_string(p, "VBR", "VBR");
	obs_property_list_add_string(p, "CQP", "CQP");
	obs_property_list_add_string(p, obs_module_text("Lossless"),
			"lossless");

	obs_property_set_modified_callback(p, rate_control_modified);

	obs_properties_add_int(props, "bitrate",
			obs_module_text("Bitrate"), 50, 300000, 50);

	obs_properties_add_int(props, "cqp", "CQP", 0, 50, 1);

	obs_properties_add_int(props, "keyint_sec",
			obs_module_text("KeyframeIntervalSec"), 0, 10, 1);

	p = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

#define add_preset(val) \
	obs_property_list_add_string(p, obs_module_text("NVENC.Preset." val), \
			val)
	add_preset("default");
	add_preset("hq");
	add_preset("hp");
	add_preset("bd");
	add_preset("ll");
	add_preset("llhq");
	add_preset("llhp");
#undef add_preset

	p = obs_properties_add_list(props, "profile", obs_module_text("Profile"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

#define add_profile(val) \
	obs_property_list_add_string(p, val, val)
	add_profile("high");
	add_profile("main");
	add_profile("baseline");
	add_profile("high444p");

	p = obs_properties_add_list(props, "level",
			obs_module_text("NVENC.Level"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_STRING);
	add_profile("auto");
	add_profile("1"   );
	add_profile("1.0" );
	add_profile("1b"  );
	add_profile("1.0b");
	add_profile("1.1" );
	add_profile("1.2" );
	add_profile("1.3" );
	add_profile("2"   );
	add_profile("2.0" );
	add_profile("2.1" );
	add_profile("2.2" );
	add_profile("3"   );
	add_profile("3.0" );
	add_profile("3.1" );
	add_profile("3.2" );
	add_profile("4"   );
	add_profile("4.0" );
	add_profile("4.1" );
	add_profile("4.2" );
	add_profile("5"   );
	add_profile("5.0" );
	add_profile("5.1" );
#undef add_profile

	obs_properties_add_bool(props, "2pass",
			obs_module_text("NVENC.Use2Pass"));
	obs_properties_add_int(props, "gpu", obs_module_text("GPU"), 0, 8, 1);

	obs_properties_add_int(props, "bf", obs_module_text("BFrames"),
			0, 4, 1);

	return props;
}

static bool nvenc_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct nvenc_encoder *enc = data;

	*extra_data = enc->header;
	*size       = enc->header_size;
	return true;
}

static bool nvenc_sei_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct nvenc_encoder *enc = data;

	*extra_data = enc->sei;
	*size       = enc->sei_size;
	return true;
}

struct obs_encoder_info nvenc_encoder_info = {
	.id             = "ffmpeg_nvenc",
	.type           = OBS_ENCODER_VIDEO,
	.codec          = "h264",
	.get_name       = nvenc_getname,
	.create         = nvenc_create,
	.destroy        = nvenc_destroy,
	.encode         = nvenc_encode,
	.get_defaults   = nvenc_defaults,
	.get_properties = nvenc_properties,
	.get_extra_data = nvenc_extra_data,
	.get_sei_data   = nvenc_sei_data,
	.get_video_info = nvenc_video_info
};
