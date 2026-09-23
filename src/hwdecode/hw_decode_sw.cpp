/*
obs-zcamera
 Copyright (C) 2019-2020 Yibai Zhang
 Copyright (C) 2026 ZCamera OBS plugin contributors

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

/* CPU (software) zero-copy-backend fallback. This is the original obs-ssp
   path: FFmpeg decodes to a CPU obs_source_frame2 and OBS uploads it. It is
   intentionally NOT zero-copy; it is the guaranteed-to-work baseline and the
   fallback whenever no hardware path is available. */

#include "hw_decode_sw.h"
#include "hw_decode.h"

#include <new>
extern "C" {
#include "ffmpeg-decode.h"
}

#include <obs-module.h>
#include <libavcodec/avcodec.h>

struct zc_hw_sw {
	struct zc_hw_decoder base;

	ffmpeg_decode vdecoder;
	bool decoder_valid;

	int width, height;
	enum video_colorspace cs;
	enum video_range_type range;
	uint8_t trc; /* enum video_trc */
	obs_source_frame2 frame;
	enum zc_codec_id codec;
};

static bool sw_decode(struct zc_hw_decoder *dec, const uint8_t *data,
		      size_t size, bool is_keyframe, int64_t pts,
		      struct zc_decoded_frame *out, bool *got_output)
{
	(void)is_keyframe;
	(void)pts;
	struct zc_hw_sw *sw = (struct zc_hw_sw *)dec;

	if (!sw->decoder_valid) {
		enum AVCodecID av_codec = (sw->codec == ZC_CODEC_HEVC)
						  ? AV_CODEC_ID_HEVC
						  : AV_CODEC_ID_H264;
		if (ffmpeg_decode_init(&sw->vdecoder, av_codec, false) < 0) {
			*got_output = false;
			return false;
		}
		sw->decoder_valid = true;
	}

	int64_t ts = pts;
	bool ok = ffmpeg_decode_video(&sw->vdecoder, (uint8_t *)data, size, &ts,
				      sw->cs, sw->range, &sw->frame,
				      got_output);

	/* The Software backend exposes the CPU frame through the zc_decoded_frame
	   wrapper: there is no GPU texture, so the source falls back to
	   obs_source_output_video2 with the frame inside priv. */
	if (ok && *got_output) {
		sw->frame.timestamp = (uint64_t)ts * 1000;
		out->backend = ZC_HW_SOFTWARE;
		out->texture = NULL;
		out->owns_texture = false;
		out->width = sw->frame.width;
		out->height = sw->frame.height;
		out->format = sw->frame.format;
		out->cs = sw->cs;
		out->range = sw->frame.range;
		out->trc = sw->frame.trc;
		out->timestamp_ns = sw->frame.timestamp;
		out->avframe = NULL;
		out->frame2 = &sw->frame;
	}
	return ok;
}

static void sw_set_params(struct zc_hw_decoder *dec, int width, int height,
			  enum video_colorspace cs, enum video_range_type range,
			  uint8_t trc)
{
	struct zc_hw_sw *sw = (struct zc_hw_sw *)dec;
	sw->width = width;
	sw->height = height;
	sw->cs = cs;
	sw->range = range;
	sw->trc = trc;
}

static bool sw_import(struct zc_hw_decoder *dec, struct zc_decoded_frame *frame)
{
	(void)dec;
	(void)frame;
	/* CPU frames have no GPU surface to import; the source outputs them via
	   obs_source_output_video2 instead. */
	return false;
}

static void sw_release(struct zc_hw_decoder *dec, struct zc_decoded_frame *frame)
{
	(void)dec;
	(void)frame;
	/* CPU frames need no GPU release. */
}

static void sw_destroy(struct zc_hw_decoder *dec)
{
	struct zc_hw_sw *sw = (struct zc_hw_sw *)dec;
	if (sw->decoder_valid) {
		ffmpeg_decode_free(&sw->vdecoder);
		sw->decoder_valid = false;
	}
	delete sw;
}

struct zc_hw_decoder *zc_hw_sw_create(enum zc_codec_id codec,
				      obs_source_t *source)
{
	(void)source;
	struct zc_hw_sw *sw = new (std::nothrow) struct zc_hw_sw;
	if (!sw)
		return NULL;
	memset(sw, 0, sizeof(*sw));

	sw->base.decode = sw_decode;
	sw->base.import = sw_import;
	sw->base.set_video_params = sw_set_params;
	sw->base.release = sw_release;
	sw->base.destroy = sw_destroy;

	sw->codec = codec;
	sw->cs = VIDEO_CS_DEFAULT;
	sw->range = VIDEO_RANGE_PARTIAL;
	sw->trc = (uint8_t)VIDEO_TRC_DEFAULT;
	return &sw->base;
}