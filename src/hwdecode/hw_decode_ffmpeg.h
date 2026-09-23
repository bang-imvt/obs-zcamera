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

/* FFmpeg-hwaccel zero-copy decode engine.

   FFmpeg's hardware-accelerated decoder is the decode engine: it auto-detects
   the best device the OS can provide (Windows: CUDA/NVDEC, D3D11VA, QSV;
   macOS: VideoToolbox). The decoded AVFrame stays on the GPU; import() then
   imports that surface directly into an OBS gs_texture with NO
   av_hwframe_transfer_data (no GPU->CPU copy) and no CPU->GPU upload. */

/* Detect a usable hardware backend for the requested hint / codec. Returns
   ZC_HW_SOFTWARE when no hardware device is usable. */
enum zc_hw_backend zc_hw_ffmpeg_detect(enum zc_hw_backend requested,
				       enum zc_codec_id codec);

/* Create an FFmpeg-hwaccel decoder for the given coded dimensions. Returns
   NULL when the hardware path cannot be set up. `ten_bit` selects a 10-bit
   (AV_PIX_FMT_P010) frames context for backends that build one (D3D11VA); the
   macOS VideoToolbox path does not force a sw_format and follows the stream. */
struct zc_hw_decoder *zc_hw_ffmpeg_create(enum zc_hw_backend backend,
					  int width, int height,
					  enum zc_codec_id codec, bool ten_bit,
					  obs_source_t *source);

#ifdef _WIN32
/* Forward-declared so this header does not have to pull in <d3d11.h> or
   <libavutil/frame.h> (and so it stays valid C, which hw_decode.c needs). */
struct ID3D11Device;
struct AVFrame;

/* Per-decoder D3D11 video-processor state: blits a decoded NV12 *array slice*
   into a shareable BGRA texture OBS can open. Opaque here; defined and owned by
   hw_decode_d3d11_win.cpp, freed by the decoder's destroy(). */
struct zc_hw_d3d11_import;
void zc_hw_d3d11_import_free(struct zc_hw_d3d11_import *imp);

/* Per-decoder CUDA/NVDEC interop state: copies the decoded CUDA NV12 planes into
   CUDA-registered R8/R8G8 D3D11 textures and converts them to a shareable BGRA
   texture OBS can open. Opaque here; defined and owned by
   hw_decode_cuda_win.cpp, freed by the decoder's destroy(). */
struct zc_hw_cuda_import;
void zc_hw_cuda_import_free(struct zc_hw_cuda_import *imp);

/* Ordinal of the CUDA device backing the D3D11 device OBS renders on, or -1 when
   it cannot be determined. */
int zc_hw_cuda_device_for_d3d11(struct ID3D11Device *device);

/* Import a decoded AV_PIX_FMT_CUDA (NVDEC) frame into an OBS texture. Runs on
   the graphics thread; `cuda_device` comes from zc_hw_cuda_device_for_d3d11(). */
gs_texture_t *zc_hw_cuda_import_frame(struct zc_hw_cuda_import **slot,
				      struct AVFrame *avf, int cuda_device,
				      struct ID3D11Device *device);
#endif

#ifdef __cplusplus
}
#endif