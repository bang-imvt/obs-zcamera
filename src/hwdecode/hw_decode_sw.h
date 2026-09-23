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

#pragma once

#include "hw_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CPU fallback backend. Reuses the original obs-ssp ffmpeg decode path and
   hands OBS an obs_source_frame2 (async frame output, no video_render). */
struct zc_hw_decoder *zc_hw_sw_create(enum zc_codec_id codec,
				      obs_source_t *source);

#ifdef __cplusplus
}
#endif