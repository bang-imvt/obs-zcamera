/*
obs-zcamera
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

/* macOS zero-copy decode: VideoToolbox straight into an OBS texture.

   Why a native VideoToolbox session instead of FFmpeg's VideoToolbox hwaccel
   (which hw_decode_ffmpeg.cpp can already drive): FFmpeg hands back a
   kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ('420v') pixel buffer, and
   libobs's OpenGL backend can only bind two IOSurface pixel formats —
   'BGRA' and 'l10r' — logging "Unexpected pixel format" for anything else
   (libobs-opengl/gl-cocoa.m, device_texture_create_from_iosurface). So a 420v
   surface cannot become a gs_texture at all, and going through it would mean a
   conversion pass of our own.

   Asking the decompression session for kCVPixelFormatType_32BGRA instead makes
   the video decoder write BGRA directly into an IOSurface, which gs binds
   natively: decode -> IOSurface -> GL_TEXTURE_RECTANGLE, with no CPU copy and
   no extra GPU pass. This mirrors what the Windows D3D11VA path ends up
   handing OBS (a BGRA texture), just without the intermediate conversion.

   10-bit / HDR streams are the exception: gs has no p010 path either (only
   'l10r', which VideoToolbox will not produce for H.264/HEVC), so those stay
   on the asynchronous CPU-frame source type (see create_ssp_source_info). */

#pragma once

#include "hw_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a VideoToolbox decoder for an Annex-B H.264/HEVC elementary stream.
   Returns NULL when no decompression session can be built (unsupported codec,
   no hardware decoder on the machine), in which case the caller falls back.
   The returned decoder's import() runs on the graphics thread, like every
   other zero-copy backend. */
struct zc_hw_decoder *zc_hw_vt_mac_create(int width, int height,
					  enum zc_codec_id codec,
					  obs_source_t *source);

#ifdef __cplusplus
}
#endif
