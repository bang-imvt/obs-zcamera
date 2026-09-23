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

/* Zero-copy hardware decode backend abstraction.

   The obs-ssp baseline decoded to a CPU obs_source_frame2 and let OBS upload
   it to the GPU. This module decodes straight into a GPU texture (NVDEC/CUDA
   on Windows, VideoToolbox/IOSurface on macOS) that OBS draws from in
   video_render, eliminating the GPU->CPU->GPU copies. The old CPU path is kept
   as the Software backend / fallback.
*/

#pragma once

#include <stdint.h>
#include <obs.h>
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

enum zc_hw_backend {
	ZC_HW_AUTO = 0,
	ZC_HW_CUDA,      /* Windows: NVDEC + D3D11 interop          */
	ZC_HW_D3D11VA,   /* Windows: D3D11VA shared surface          */
	ZC_HW_VIDEOTOOLBOX, /* macOS: VideoToolbox + IOSurface      */
	ZC_HW_SOFTWARE,  /* CPU fallback (FFmpeg, optional)          */
	ZC_HW_COUNT
};

/* Neutral codec id so the zero-copy decoders do not depend on FFmpeg. */
enum zc_codec_id {
	ZC_CODEC_H264 = 1,
	ZC_CODEC_HEVC = 2,
	ZC_CODEC_COUNT,
};

const char *zc_hw_backend_name(enum zc_hw_backend backend);

/* A decoded frame. `texture` is a GPU texture ready to draw with
   obs_source_draw(): it is filled by import() which MUST run on the graphics
   thread. For a CPU (Software) frame, `texture` is null and `frame2` points at
   an obs_source_frame2 to feed via obs_source_output_video2. */
struct zc_decoded_frame {
	enum zc_hw_backend backend;   /* which backend produced this frame   */
	gs_texture_t *texture;        /* GPU texture to draw (owned by decoder) */
	bool owns_texture;            /* true when decoder must release it       */
	uint32_t width, height;
	enum video_format format;      /* NV12 / P010 / etc.               */
	enum video_colorspace cs;
	enum video_range_type range;
	uint8_t trc; /* enum video_trc (matches obs_source_frame2) */
	uint64_t timestamp_ns;         /* presentation timestamp           */
	/* Either the hardware AVFrame (GPU) or the software obs_source_frame2. */
	void *avframe;                 /* AVFrame* for HW backends          */
	struct obs_source_frame2 *frame2; /* CPU frame for Software backend  */
};

/* Backend interface implemented per platform. */
struct zc_hw_decoder {
	/* Decode one access unit. On `got_output`, fields of `out` are filled
	   (avframe or frame2 set). Caller draws then calls release(). */
	bool (*decode)(struct zc_hw_decoder *dec, const uint8_t *data,
		       size_t size, bool is_keyframe, int64_t pts,
		       struct zc_decoded_frame *out, bool *got_output);

	/* Import a decoded HW frame into an OBS texture. MUST run on the
	   graphics thread. Fills out->texture. Returns false when the GPU
	   surface cannot be imported (e.g. unsupported pixel format). */
	bool (*import)(struct zc_hw_decoder *dec, struct zc_decoded_frame *frame);

	/* Set the coded frame size when signaling from meta changes. */
	void (*set_video_params)(struct zc_hw_decoder *dec, int width,
				 int height, enum video_colorspace cs,
				 enum video_range_type range, uint8_t trc);

	/* Release a frame previously returned by decode(). Must run on the
	   graphics thread if the frame owns a texture. */
	void (*release)(struct zc_hw_decoder *dec,
			struct zc_decoded_frame *frame);

	/* Destroy the decoder (graphics thread). */
	void (*destroy)(struct zc_hw_decoder *dec);
};

enum zc_hw_backend zc_hw_resolve(enum zc_hw_backend requested,
				 enum zc_codec_id codec);
/* Create a decoder. `ten_bit` selects a 10-bit (P010) frames context for
   hardware backends that build one (D3D11VA); software decode ignores it (the
   CPU decoder adapts to whatever the stream really is). */
struct zc_hw_decoder *zc_hw_decoder_create(enum zc_hw_backend backend,
					   int width, int height,
					   enum zc_codec_id codec, bool ten_bit,
					   obs_source_t *source);
void zc_hw_decoder_destroy(struct zc_hw_decoder *dec);

/* Detect the luma bit depth (8, 10, ...) from the SPS carried in an
   H.264/HEVC access unit; returns 0 when no SPS is present (unknown). The SSP
   video meta does not advertise bit depth, so the source must sniff the
   elementary stream. Handles Annex-B and length-prefixed (AVCC) layouts. */
int zc_nal_bit_depth(const uint8_t *data, size_t size, enum zc_codec_id codec);

#ifdef __cplusplus
}
#endif