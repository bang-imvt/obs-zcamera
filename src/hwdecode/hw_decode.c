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

#include "hw_decode.h"
#ifdef ENABLE_SW_DECODE
#include "hw_decode_sw.h"
#endif
#ifdef ENABLE_ZERO_COPY
#include "hw_decode_ffmpeg.h"
#endif

const char *zc_hw_backend_name(enum zc_hw_backend backend)
{
	switch (backend) {
	case ZC_HW_AUTO:
		return "Auto";
	case ZC_HW_CUDA:
		return "CUDA (NVDEC)";
	case ZC_HW_D3D11VA:
		return "D3D11VA";
	case ZC_HW_VIDEOTOOLBOX:
		return "VideoToolbox";
	case ZC_HW_SOFTWARE:
		return "Software";
	default:
		return "Unknown";
	}
}

/* Resolve a concrete, usable backend. Hardware decode goes through the
   FFmpeg-hwaccel engine (covers any GPU the OS recognizes); Software is only
   the fallback when no hardware path is usable. */
enum zc_hw_backend zc_hw_resolve(enum zc_hw_backend requested,
				 enum zc_codec_id codec)
{
#ifdef ENABLE_ZERO_COPY
	if (requested != ZC_HW_SOFTWARE) {
		enum zc_hw_backend hw = zc_hw_ffmpeg_detect(requested, codec);
		if (hw != ZC_HW_SOFTWARE)
			return hw;
	}
#else
	(void)requested;
	(void)codec;
#endif
#ifdef ENABLE_SW_DECODE
	return ZC_HW_SOFTWARE;
#else
	return ZC_HW_COUNT; /* no decoder available on this machine */
#endif
}

struct zc_hw_decoder *zc_hw_decoder_create(enum zc_hw_backend backend,
					   int width, int height,
					   enum zc_codec_id codec, bool ten_bit,
					   obs_source_t *source)
{
#ifdef ENABLE_ZERO_COPY
	if (backend != ZC_HW_SOFTWARE && backend != ZC_HW_COUNT) {
		struct zc_hw_decoder *hw = zc_hw_ffmpeg_create(backend, width,
							       height, codec,
							       ten_bit, source);
		if (hw)
			return hw;
		blog(LOG_WARNING,
		     "[obs-zcamera] zero-copy decode unavailable, "
		     "falling back to software");
	}
#else
	(void)backend;
	(void)width;
	(void)height;
	(void)ten_bit;
#endif
#ifdef ENABLE_SW_DECODE
	return zc_hw_sw_create(codec, source);
#else
	(void)codec;
	(void)source;
	return NULL;
#endif
}

void zc_hw_decoder_destroy(struct zc_hw_decoder *dec)
{
	if (dec && dec->destroy)
		dec->destroy(dec);
}

/* ------------------------------------------------------------------------- */
/* Elementary-stream bit-depth sniffing (H.264 / HEVC SPS)                    */
/* ------------------------------------------------------------------------- */

/* The SSP video meta carries no bit depth, so the source detects 10-bit from
   the SPS of the elementary stream. This is a small, defensive RBSP parser:
   it only needs profile/bit-depth fields, never full syntax, and it aborts
   (returns "unknown") the moment the layout does not match expectations. */

struct zc_bitreader {
	const uint8_t *data;
	size_t size; /* bytes in the RBSP view */
	size_t pos;  /* bit position */
};

static uint64_t zc_br_read(struct zc_bitreader *br, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++) {
		if ((br->pos >> 3) >= br->size)
			return 0;
		uint8_t byte = br->data[br->pos >> 3];
		int bit = 7 - (int)(br->pos & 7);
		v = (v << 1) | ((byte >> bit) & 1);
		br->pos++;
	}
	return v;
}

/* Exp-Golomb (unsigned) ue(v). */
static uint64_t zc_br_ue(struct zc_bitreader *br)
{
	int zeros = 0;
	while (zc_br_read(br, 1) == 0) {
		zeros++;
		if (zeros > 31)
			return 0;
	}
	if (zeros == 0)
		return 0;
	return ((uint64_t)1 << zeros) - 1 + zc_br_read(br, zeros);
}

/* Find the NAL units in an Annex-B access unit: returns the offset of the
   first NAL payload after a 00 00 01 / 00 00 00 01 start code, or SIZE_MAX. */
static size_t zc_nal_find_start(const uint8_t *d, size_t size, size_t *offset)
{
	size_t i = *offset;
	while (i + 3 <= size) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			*offset = i + 3;
			return *offset;
		}
		if (i + 4 <= size && d[i] == 0 && d[i + 1] == 0 &&
		    d[i + 2] == 0 && d[i + 3] == 1) {
			*offset = i + 4;
			return *offset;
		}
		i++;
	}
	return SIZE_MAX;
}

/* Build an RBSP view of a NAL payload, removing emulation-prevention bytes
   (00 00 03 -> 00 00). `out` must hold at least `len` bytes. */
static size_t zc_nal_to_rbsp(const uint8_t *nal, size_t len, uint8_t *out)
{
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 &&
		    nal[i + 2] == 3) {
			out[n++] = 0;
			out[n++] = 0;
			i += 2; /* skip the 0x03 */
			continue;
		}
		out[n++] = nal[i];
	}
	return n;
}

/* H.264 SPS: returns the luma bit depth, or 0 on parse failure. */
static int zc_sps_h264_bit_depth(const uint8_t *rbsp, size_t size)
{
	struct zc_bitreader br = {rbsp, size, 0};
	uint64_t profile_idc = zc_br_read(&br, 8);
	(void)zc_br_read(&br, 8); /* constraint flags */
	(void)zc_br_read(&br, 8); /* level_idc */
	(void)zc_br_ue(&br);      /* seq_parameter_set_id */

	/* Profiles with extended bit depths (High, High 10, High 4:2:2, High
	   4:4:4, ...). Baseline/Main/High-8 are always 8-bit. */
	switch (profile_idc) {
	case 44:
	case 83:
	case 86:
	case 100:
	case 110:
	case 118:
	case 122:
	case 128:
	case 134:
	case 138:
	case 139:
	case 244:
		break;
	default:
		return 8;
	}
	uint64_t chroma_format_idc = zc_br_ue(&br);
	if (chroma_format_idc == 3)
		(void)zc_br_read(&br, 1); /* separate_colour_plane_flag */
	uint64_t bit_depth_luma_minus8 = zc_br_ue(&br);
	return 8 + (int)bit_depth_luma_minus8;
}

/* HEVC SPS: returns the luma bit depth, or 0 on parse failure. */
static int zc_sps_hevc_bit_depth(const uint8_t *rbsp, size_t size)
{
	struct zc_bitreader br = {rbsp, size, 0};
	(void)zc_br_read(&br, 4); /* sps_video_parameter_set_id */
	uint64_t max_sub_layers_minus1 = zc_br_read(&br, 3);
	(void)zc_br_read(&br, 1); /* sps_temporal_id_nesting_flag */

	/* profile_tier_level(1, max_sub_layers_minus1) */
	(void)zc_br_read(&br, 2);  /* general_profile_space */
	(void)zc_br_read(&br, 1);  /* general_tier_flag */
	(void)zc_br_read(&br, 5);  /* general_profile_idc */
	(void)zc_br_read(&br, 32); /* general_profile_compatibility_flag */
	(void)zc_br_read(&br, 1);  /* general_progressive_source_flag */
	(void)zc_br_read(&br, 1);  /* general_interlaced_source_flag */
	(void)zc_br_read(&br, 1);  /* general_non_packed_constraint_flag */
	(void)zc_br_read(&br, 1);  /* general_frame_only_constraint_flag */
	(void)zc_br_read(&br, 44); /* general_reserved_zero_44bits */
	(void)zc_br_read(&br, 8);  /* general_level_idc */

	for (uint64_t i = 0; i < max_sub_layers_minus1; i++) {
		(void)zc_br_read(&br, 2); /* reserved_zero_2bits */
		uint64_t profile_present = zc_br_read(&br, 1);
		uint64_t level_present = zc_br_read(&br, 1);
		if (profile_present) {
			(void)zc_br_read(&br, 2);
			(void)zc_br_read(&br, 1);
			(void)zc_br_read(&br, 5);
			(void)zc_br_read(&br, 32);
			(void)zc_br_read(&br, 1);
			(void)zc_br_read(&br, 1);
			(void)zc_br_read(&br, 1);
			(void)zc_br_read(&br, 1);
			(void)zc_br_read(&br, 44);
		}
		if (level_present)
			(void)zc_br_read(&br, 8);
	}

	(void)zc_br_ue(&br); /* sps_seq_parameter_set_id */
	uint64_t chroma_format_idc = zc_br_ue(&br);
	if (chroma_format_idc == 3) /* separate_colour_plane_flag (4:4:4) */
		(void)zc_br_read(&br, 1);
	(void)zc_br_ue(&br); /* pic_width_in_luma_samples */
	(void)zc_br_ue(&br); /* pic_height_in_luma_samples */
	if (zc_br_read(&br, 1)) { /* conformance_window_flag */
		(void)zc_br_ue(&br);
		(void)zc_br_ue(&br);
		(void)zc_br_ue(&br);
		(void)zc_br_ue(&br);
	}
	uint64_t bit_depth_luma_minus8 = zc_br_ue(&br);
	return 8 + (int)bit_depth_luma_minus8;
}

int zc_nal_bit_depth(const uint8_t *data, size_t size, enum zc_codec_id codec)
{
	if (!data || size < 4)
		return 0;

	const bool hevc = (codec == ZC_CODEC_HEVC);
	const int sps_type = hevc ? 33 : 7;
	const size_t hdr_len = hevc ? 2 : 1;

	/* Emulation-prevention-removed RBSP for the SPS payload. SPS units are
	   small; a fixed scratch buffer is fine (an oversized/corrupt unit just
	   reports "unknown" instead of overflowing). */
	uint8_t rbsp[2048];

#define ZC_TRY_SPS(nal, len)                                                  \
	do {                                                                  \
		if ((len) <= hdr_len)                                          \
			break;                                                \
		const uint8_t *nal_payload = (nal) + hdr_len;                  \
		size_t payload_len = (len) - hdr_len;                          \
		if (payload_len > sizeof(rbsp))                                \
			break;                                                \
		size_t rbsp_len = zc_nal_to_rbsp(nal_payload, payload_len,     \
						 rbsp);                        \
		if (rbsp_len == 0)                                            \
			break;                                                \
		if (!hevc)                                                    \
			return zc_sps_h264_bit_depth(rbsp, rbsp_len);          \
		else                                                          \
			return zc_sps_hevc_bit_depth(rbsp, rbsp_len);          \
	} while (0)

	/* Annex-B: 00 00 01 / 00 00 00 01 start codes. */
	{
		size_t off = 0;
		size_t nal_start = zc_nal_find_start(data, size, &off);
		while (nal_start != SIZE_MAX) {
			size_t nal_end = nal_start;
			size_t next = zc_nal_find_start(data, size, &nal_end);
			size_t nal_len = (next == SIZE_MAX) ? size - nal_start
							    : next - nal_start;
			if (nal_len >= hdr_len) {
				int nal_type =
					hevc ? ((data[nal_start] >> 1) & 0x3f)
					     : (data[nal_start] & 0x1f);
				if (nal_type == sps_type)
					ZC_TRY_SPS(data + nal_start, nal_len);
			}
			if (next == SIZE_MAX)
				break;
			off = next;
			nal_start = zc_nal_find_start(data, size, &off);
		}
	}

	/* Length-prefixed (AVCC / HVCC style): 4-byte big-endian NAL length. */
	{
		size_t off = 0;
		while (off + 4 <= size) {
			size_t nal_len = ((size_t)data[off] << 24) |
					 ((size_t)data[off + 1] << 16) |
					 ((size_t)data[off + 2] << 8) |
					 (size_t)data[off + 3];
			off += 4;
			if (nal_len == 0 || nal_len > size - off)
				return 0;
			int nal_type = (hevc && nal_len >= 2)
						? ((data[off] >> 1) & 0x3f)
						: (data[off] & 0x1f);
			if (nal_type == sps_type)
				ZC_TRY_SPS(data + off, nal_len);
			off += nal_len;
		}
	}
#undef ZC_TRY_SPS
	return 0;
}