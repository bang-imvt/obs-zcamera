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

/* FFmpeg-hwaccel zero-copy decoder.

   Decodes H.264/HEVC with FFmpeg's hardware device (auto-detected to cover any
   GPU the OS provides), keeps the resulting AVFrame on the GPU, and import()
   wraps the surface into an OBS gs_texture with NO av_hwframe_transfer_data:

     Windows: D3D11VA decoder outputs a D3D11 texture that we share and import
              via gs_texture_open_shared(); CUDA/NVDEC frames are copied into
              CUDA-registered D3D11 planes and converted to a shared D3D11
              texture the same way.
     macOS:   VideoToolbox outputs a CVPixelBuffer (IOSurface) imported via
              gs_texture_create_from_iosurface().

   The real (hardware-dependent) device/texture setup lives behind the
   device-context accessor helpers below; where a platform cannot provide a
   shared surface at build time, decode() still fills avframe and import()
   returns false so the caller falls back to Software. */

#include "hw_decode_ffmpeg.h"

#include <obs-module.h>
#include <obs.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

#if defined(_WIN32)
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#elif defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

#include <cstring>
#include <cstdio>
#include <new>

namespace zc {

/* Forward declarations. */
static void ffmpeg_destroy(struct zc_hw_decoder *dec);
#ifdef _WIN32
gs_texture_t *zc_hw_import_d3d11_slice(struct zc_hw_d3d11_import **slot,
				       AVFrame *avf, int slice);
#endif

struct zc_hw_ffmpeg {
	struct zc_hw_decoder base;

	enum zc_hw_backend backend;
	enum zc_codec_id codec;
	/* 10-bit decode requested: the D3D11VA frames context is built with
	   AV_PIX_FMT_P010 (DXGI P010 surfaces) instead of NV12, so a 10-bit
	   stream is not silently crushed to 8-bit. */
	bool ten_bit = false;
	/* The YUV format the frames context was built with (NV12 or P010).
	   Hardware frames report AV_PIX_FMT_D3D11/VIDEOTOOLBOX/CUDA, so this is
	   what zc_decoded_frame.format must be derived from (or the negotiated
	   sw_format from the frame's hw_frames_ctx at decode time). */
	enum AVPixelFormat hw_sw_fmt = AV_PIX_FMT_NV12;

	/* FFmpeg decoder state. */
	const AVCodec *avcodec = nullptr;
	AVCodecContext *avctx = nullptr;
	AVBufferRef *hw_device_ctx = nullptr;
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
	AVFrame *hw_frame = nullptr;
	AVFrame *sw_frame = nullptr;

	int width = 0, height = 0;
	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	enum video_range_type range = VIDEO_RANGE_PARTIAL;
	uint8_t trc = (uint8_t)VIDEO_TRC_DEFAULT;

	/* Diagnostic/latch state. Deliberately per-decoder members: get_format and
	   import run on decoder threads, so a function-local `static` here would be
	   shared (and mutated racily) by every decoder instance. */
	bool logged_formats = false;
	bool logged_non_hw_frame = false;
	/* Set once the decoded surface proved un-importable, so the attempt (and
	   its log line) does not repeat at stream frame rate. */
	bool import_unsupported = false;

#ifdef _WIN32
	/* Video processor + shared BGRA texture used to import a decoded NV12
	   array slice (see hw_decode_d3d11_win.cpp). Owned by this decoder. */
	struct zc_hw_d3d11_import *d3d11_import = nullptr;

	/* CUDA/NVDEC interop state, plus the CUDA ordinal backing the OBS adapter
	   (the importer creates its own context on that device; see
	   hw_decode_cuda_win.cpp). Owned by this decoder. */
	struct zc_hw_cuda_import *cuda_import = nullptr;
	int cuda_device = -1;
#endif
};

/* Map our neutral codec id to an AV codec id. */
static AVCodecID zc_to_av_codec(enum zc_codec_id c)
{
	switch (c) {
	case ZC_CODEC_HEVC:
		return AV_CODEC_ID_HEVC;
	case ZC_CODEC_H264:
	default:
		return AV_CODEC_ID_H264;
	}
}

/* avcodec get_format callback: force the hardware pixel format so the decoder
   actually produces hardware (D3D11 / VideoToolbox) frames instead of falling
   back to a software pix_fmt. Without this, avcodec picks the first format it
   finds (e.g. yuv420p) and never uses our shared frames context. */
static enum AVPixelFormat zc_get_format(AVCodecContext *avctx,
					const enum AVPixelFormat *pix_fmts)
{
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)avctx->opaque;
	enum AVPixelFormat hw_fmt = f ? f->hw_pix_fmt : AV_PIX_FMT_NONE;
	/* First call for this decoder only: report what codec formats are on
	   offer. Kept per-decoder (not a function-local static) because get_format
	   runs on decoder threads and several decoders can coexist. */
	if (f && !f->logged_formats) {
		f->logged_formats = true;
		char names[256] = {0};
		int pos = 0;
		for (const enum AVPixelFormat *p = pix_fmts;
		     *p != AV_PIX_FMT_NONE; p++) {
			const char *nm = av_get_pix_fmt_name(*p);
			int n = snprintf(names + pos, sizeof(names) - pos,
					 "%s%s", nm ? nm : "?", pos ? " " : "");
			if (n <= 0)
				break;
			pos += n;
		}
		blog(LOG_INFO,
		     "[obs-zcamera] get_format: hw_fmt=%s profile=%d level=%d "
		     "coded=%dx%d offered=[%s]",
		     hw_fmt == AV_PIX_FMT_NONE
			     ? "(none)"
			     : av_get_pix_fmt_name(hw_fmt),
		     avctx->profile, avctx->level, avctx->coded_width,
		     avctx->coded_height, names);
	}
	if (hw_fmt == AV_PIX_FMT_NONE)
		return pix_fmts[0];
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == hw_fmt)
			break;
		else if (p[1] == AV_PIX_FMT_NONE)
			return pix_fmts[0];
	}

	/* FFmpeg's D3D11VA whitelist covers Constrained Baseline/Main/High only,
	   so a plain H.264 Baseline stream (what many IP cameras send) is refused
	   and the decoder silently falls back to software. Baseline is a subset of
	   Main, so the hardware decoder handles it; let the format through.
	   This must happen here, while get_format runs: the whitelist is consulted
	   when FFmpeg initialises the hwaccel, right after this returns. */
	if (hw_fmt == AV_PIX_FMT_D3D11 && f->codec == ZC_CODEC_H264 &&
	    avctx->profile == AV_PROFILE_H264_BASELINE) {
		avctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
		blog(LOG_INFO,
		     "[obs-zcamera] H.264 baseline is not on FFmpeg's D3D11VA "
		     "profile list; decoding it on the GPU anyway");
	}
	return hw_fmt;
}

/* Map an FFmpeg pixel format to an OBS video format. */
static enum video_format zc_av_to_obs_format(AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_NONE:
		return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUV420P10LE:
	case AV_PIX_FMT_YUV420P10BE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
	case AV_PIX_FMT_P010BE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	default:
		return VIDEO_FORMAT_NV12; /* hw surfaces are typically NV12 */
	}
}

/* Map the requested backend to the FFmpeg hwaccel device type. */
static enum AVHWDeviceType zc_to_hw_type(enum zc_hw_backend backend)
{
	switch (backend) {
#ifdef _WIN32
	case ZC_HW_CUDA:
		return AV_HWDEVICE_TYPE_CUDA;
	case ZC_HW_D3D11VA:
		return AV_HWDEVICE_TYPE_D3D11VA;
#endif
#ifdef __APPLE__
	case ZC_HW_VIDEOTOOLBOX:
		return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#endif
	default:
		return AV_HWDEVICE_TYPE_NONE;
	}
}

const char *zc_hw_ffmpeg_type_name(enum AVHWDeviceType type)
{
	switch (type) {
	case AV_HWDEVICE_TYPE_CUDA:
		return "CUDA (NVDEC)";
	case AV_HWDEVICE_TYPE_D3D11VA:
		return "D3D11VA";
	case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
		return "VideoToolbox";
	case AV_HWDEVICE_TYPE_QSV:
		return "Intel QSV";
	case AV_HWDEVICE_TYPE_DXVA2:
		return "DXVA2";
	default:
		return "unknown";
	}
}

/* Find the first device type, in priority order, that the decoder codec
   supports. Cross-vendor; Windows tries CUDA, D3D11VA, QSV; macOS VideoToolbox. */
static enum AVHWDeviceType pick_hw_device(struct zc_hw_ffmpeg *f,
					  enum zc_hw_backend requested)
{
	const AVCodec *codec = avcodec_find_decoder(zc_to_av_codec(f->codec));
	if (!codec)
		return AV_HWDEVICE_TYPE_NONE;

	/* Explicit backend request. */
	enum AVHWDeviceType explicit_type = zc_to_hw_type(requested);
	if (explicit_type != AV_HWDEVICE_TYPE_NONE)
		return explicit_type;

	/* Auto: pick the first device we can actually create AND import into OBS.
	   D3D11VA stays first because its import path needs no vendor driver API;
	   CUDA/NVDEC has a working import too and is offered as an explicit choice,
	   while QSV/DXVA2 have no import implementation and are never selected. */
	static const enum AVHWDeviceType priority[] = {
#ifdef _WIN32
		AV_HWDEVICE_TYPE_D3D11VA,
#elif defined(__APPLE__)
		AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
		AV_HWDEVICE_TYPE_NONE,
#endif
	};

	for (int i = 0; priority[i] != AV_HWDEVICE_TYPE_NONE; i++) {
		for (int j = 0;; j++) {
			const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, j);
			if (!cfg)
				break;
			if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			    cfg->device_type == priority[i]) {
				/* Verify we can actually open the device, otherwise
				   fall through to the next candidate (fixes AMD/Intel
				   machines where CUDA is "supported" but absent). */
				AVBufferRef *test = NULL;
				if (av_hwdevice_ctx_create(&test, priority[i], NULL,
							   NULL, 0) >= 0) {
					av_buffer_unref(&test);
					return priority[i];
				}
				break;
			}
		}
	}
	return AV_HWDEVICE_TYPE_NONE;
}

/* Get the OBS graphics device object (ID3D11Device on Windows). */
static void *zc_obs_device()
{
	/* Called inside obs_enter_graphics()/obs_leave_graphics(). */
	void *device = gs_get_device_obj();
	if (!device)
		blog(LOG_WARNING, "[obs-zcamera] no OBS graphics device");
	return device;
}

#ifdef _WIN32
/* FFmpeg's device-context lock callbacks, for the D3D11VA context that wraps
   the OBS device.

   A D3D11 immediate context is not thread-safe, and here two threads share one:
   the receive thread (FFmpeg decoding) and the OBS graphics thread (rendering,
   including our import). Both entering the driver at once deadlocks inside it,
   which is what froze the source permanently. FFmpeg calls these callbacks
   around every device_context/video_context call and documents them as
   protecting exactly that access; it also requires the lock to be recursive.
   obs_enter_graphics() is that lock: libobs holds it for the whole render,
   libobs-d3d11's device_enter_context is a no-op, and the mutex is recursive
   per thread. So the two threads are serialized without a second lock. */
static void zc_d3d11_lock(void *lock_ctx)
{
	(void)lock_ctx;
	obs_enter_graphics();
}

static void zc_d3d11_unlock(void *lock_ctx)
{
	(void)lock_ctx;
	obs_leave_graphics();
}
#endif

/* ---- interface: decode ---- */

static bool ffmpeg_decode(struct zc_hw_decoder *dec, const uint8_t *data,
			  size_t size, bool is_keyframe, int64_t pts,
			  struct zc_decoded_frame *out, bool *got_output)
{
	(void)is_keyframe;
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)dec;
	*got_output = false;

	if (!f->avctx)
		return false;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;
	if (av_new_packet(pkt, (int)size) < 0) {
		av_packet_free(&pkt);
		return false;
	}
	memcpy(pkt->data, data, size);
	pkt->pts = pts;
	pkt->dts = pts;

	int ret = avcodec_send_packet(f->avctx, pkt);
	av_packet_free(&pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN))
		return false;

	ret = avcodec_receive_frame(f->avctx, f->hw_frame);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		return true;
	if (ret < 0)
		return false;

	/* Hand the GPU frame to the caller as an owned ref (decoder's single
	   hw_frame is reused; we copy the reference so the source controls when
	   it is released). */
	AVFrame *owned = av_frame_alloc();
	if (!owned)
		return false;
	av_frame_ref(owned, f->hw_frame);

	out->backend = f->backend;
	out->texture = NULL;
	out->owns_texture = false;
	out->width = (uint32_t)(owned->width ? owned->width : f->width);
	out->height = (uint32_t)(owned->height ? owned->height : f->height);
	/* Hardware frames report AV_PIX_FMT_D3D11 / VIDEOTOOLBOX / CUDA, not the
	   YUV format underneath. Derive VIDEO_FORMAT_P010 vs NV12 from the frames
	   context sw_format the decoder negotiated for this surface; a software
	   frame carries its real pixel format in owned->format directly. */
	AVPixelFormat yuv_fmt = (AVPixelFormat)owned->format;
	if (owned->hw_frames_ctx) {
		AVHWFramesContext *fc = (AVHWFramesContext *)
					      owned->hw_frames_ctx->data;
		if (fc)
			yuv_fmt = fc->sw_format;
	}
	out->format = zc_av_to_obs_format(yuv_fmt);
	out->cs = f->cs;
	out->range = f->range;
	out->trc = f->trc;
	out->timestamp_ns = (uint64_t)(pts >= 0 ? pts : 0) * 1000;
	out->avframe = owned;
	out->frame2 = NULL;
	*got_output = true;
	return true;
}

/* ---- interface: import (graphics thread) ---- */

/* Wrap the decoded GPU surface into an OBS texture without a CPU copy. */
static bool ffmpeg_import(struct zc_hw_decoder *dec,
			  struct zc_decoded_frame *frame)
{
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)dec;
	if (!frame->avframe)
		return false;

	AVFrame *avf = (AVFrame *)frame->avframe;

#if defined(__APPLE__)
	/* VideoToolbox gives data[3] as a CVPixelBufferRef. OBS needs the
	   IOSurfaceRef behind it, not the CVPixelBuffer pointer itself. */
	if (avf->format == AV_PIX_FMT_VIDEOTOOLBOX && avf->data[3]) {
		CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)avf->data[3];
		IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel_buffer);
		if (surface) {
			obs_enter_graphics();
			gs_texture_t *tex =
				gs_texture_create_from_iosurface((void *)surface);
			obs_leave_graphics();
			if (tex) {
				frame->texture = tex;
				frame->owns_texture = true;
				return true;
			}
		}
	}
	return false;
#endif

#if defined(_WIN32)
	/* D3D11VA surfaces and CUDA/NVDEC frames are importable; anything else
	   falls back. A CUDA frame has data[0]/data[1] as CUdeviceptr, NOT a
	   texture — routing it to the D3D11 importer would crash, so each format
	   gets its own importer. */
	if (avf->format == AV_PIX_FMT_D3D11) {
		/* The D3D11VA decoder hands back one element of an NV12 *texture
		   array*. Neither the array nor NV12 can be opened by OBS directly,
		   so zc_hw_import_d3d11_slice() blits the slice into a shareable
		   non-array BGRA texture with an ID3D11VideoProcessor and opens that
		   (no GPU->CPU copy). See hw_decode_d3d11_win.cpp. */
		if (f->import_unsupported)
			return false;
		int slice = (int)(intptr_t)avf->data[1]; /* D3D11 array slice */
		obs_enter_graphics();
		gs_texture_t *tex =
			zc_hw_import_d3d11_slice(&f->d3d11_import, avf, slice);
		obs_leave_graphics();
		if (tex) {
			frame->texture = tex;
			frame->owns_texture = true;
			/* The blit converts to BGRA, whatever the decoder produced. */
			frame->format = VIDEO_FORMAT_BGRA;
			return true;
		}
		f->import_unsupported = true;
		blog(LOG_WARNING,
		     "[obs-zcamera] D3D11VA surface (NV12 array slice %d) could "
		     "not be imported into OBS; using software decode for this "
		     "connection",
		     slice);
		return false;
	}
	if (avf->format == AV_PIX_FMT_CUDA) {
		/* NVDEC leaves the NV12 planes in device memory, which OBS can neither
		   sample nor open. zc_hw_cuda_import_frame() copies both planes into
		   CUDA-registered R8/R8G8 D3D11 textures and runs a YUV->RGBA pass
		   into a shareable BGRA texture — again with no CPU copy. See
		   hw_decode_cuda_win.cpp. */
		if (f->import_unsupported)
			return false;
		obs_enter_graphics();
		gs_texture_t *tex = zc_hw_cuda_import_frame(
			&f->cuda_import, avf, f->cuda_device,
			(ID3D11Device *)zc_obs_device());
		obs_leave_graphics();
		if (tex) {
			frame->texture = tex;
			frame->owns_texture = true;
			/* The conversion pass produces BGRA, whatever NVDEC decoded. */
			frame->format = VIDEO_FORMAT_BGRA;
			return true;
		}
		f->import_unsupported = true;
		blog(LOG_WARNING,
		     "[obs-zcamera] CUDA/NVDEC frame could not be imported into OBS; "
		     "using software decode for this connection");
		return false;
	}
	/* Diagnostic: why are we not getting an importable surface? Once per
	   decoder, not per frame (import runs at stream frame rate). */
	if (!f->logged_non_hw_frame) {
		f->logged_non_hw_frame = true;
		blog(LOG_WARNING,
		     "[obs-zcamera] hardware decoder produced %s "
		     "(hw_frames_ctx=%s); expected D3D11 or CUDA for zero-copy import",
		     av_get_pix_fmt_name((enum AVPixelFormat)avf->format),
		     (f->avctx && f->avctx->hw_frames_ctx) ? "yes" : "no");
	}
	return false;
#endif

	return false;
}

/* ---- interface: release ---- */

static void ffmpeg_release(struct zc_hw_decoder *dec,
			   struct zc_decoded_frame *frame)
{
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)dec;
	if (frame->owns_texture && frame->texture) {
		obs_enter_graphics();
		gs_texture_destroy(frame->texture);
		obs_leave_graphics();
		frame->texture = NULL;
		frame->owns_texture = false;
	}
	/* Free the caller-owned AVFrame handed out by decode(). The decoder's
	   internal hw_frame is managed by avcodec itself. */
	if (frame->avframe) {
		av_frame_free((AVFrame **)&frame->avframe);
		frame->avframe = NULL;
	}
	(void)f;
}

static void ffmpeg_set_params(struct zc_hw_decoder *dec, int width, int height,
			      enum video_colorspace cs,
			      enum video_range_type range, uint8_t trc)
{
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)dec;
	f->width = width;
	f->height = height;
	f->cs = cs;
	f->range = range;
	f->trc = trc;
}

static void ffmpeg_destroy(struct zc_hw_decoder *dec)
{
	struct zc_hw_ffmpeg *f = (struct zc_hw_ffmpeg *)dec;
#ifdef _WIN32
	/* Releases the video processor, the shared BGRA texture and the D3D11
	   references it holds. Must run on the graphics thread, like import(). */
	zc_hw_d3d11_import_free(f->d3d11_import);
	f->d3d11_import = nullptr;
	/* Same threading rule. It also owns the CUDA context its views live in, so
	   this has to happen before the D3D11 references go away. */
	zc_hw_cuda_import_free(f->cuda_import);
	f->cuda_import = nullptr;
	f->cuda_device = -1;
#endif
	if (f->hw_frame)
		av_frame_free(&f->hw_frame);
	if (f->sw_frame)
		av_frame_free(&f->sw_frame);
	if (f->avctx)
		avcodec_free_context(&f->avctx);
	if (f->hw_device_ctx)
		av_buffer_unref(&f->hw_device_ctx);
	delete f;
}

/* ---- factory ---- */

} // namespace zc

enum zc_hw_backend zc_hw_ffmpeg_detect(enum zc_hw_backend requested,
				       enum zc_codec_id codec)
{
	zc::zc_hw_ffmpeg probe;
	probe.codec = codec;
	probe.backend = requested;
	enum AVHWDeviceType type = zc::pick_hw_device(&probe, requested);
	if (type == AV_HWDEVICE_TYPE_NONE)
		return ZC_HW_SOFTWARE;
	switch (type) {
	case AV_HWDEVICE_TYPE_CUDA:
		return ZC_HW_CUDA;
	case AV_HWDEVICE_TYPE_D3D11VA:
		return ZC_HW_D3D11VA;
	case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
		return ZC_HW_VIDEOTOOLBOX;
	default:
		return ZC_HW_SOFTWARE;
	}
}

struct zc_hw_decoder *zc_hw_ffmpeg_create(enum zc_hw_backend backend,
					  int width, int height,
					  enum zc_codec_id codec, bool ten_bit,
					  obs_source_t *source)
{
	(void)source;
	zc::zc_hw_ffmpeg *f = new (std::nothrow) zc::zc_hw_ffmpeg;
	if (!f)
		return nullptr;

	f->backend = backend;
	f->codec = codec;
	f->ten_bit = ten_bit;
	f->width = width;
	f->height = height;

	/* Select and create the FFmpeg hardware device, so the decoder opens
	   on the GPU right away. This is where the OBS device can be handed to
	   the hw context so decode shares the same adapter. */
	enum AVHWDeviceType hw_type = zc::pick_hw_device(f, backend);
	if (hw_type == AV_HWDEVICE_TYPE_NONE) {
		delete f;
		return nullptr;
	}

#if defined(_WIN32)
	/* D3D11VA only: prefer the OBS D3D11 device so decoded surfaces live on
	   the same adapter OBS renders on (required for zero-copy sharing). CUDA
	   cannot be handed a D3D11 device — it is pinned to the matching adapter by
	   ordinal below instead. */
	if (hw_type == AV_HWDEVICE_TYPE_D3D11VA) {
		void *obs_dev = zc::zc_obs_device();
		if (obs_dev) {
			/* Bind the OBS device to the hw context, then init it. */
			AVBufferRef *ref = av_hwdevice_ctx_alloc(hw_type);
			if (ref) {
				AVHWDeviceContext *hwctx = (AVHWDeviceContext *)ref->data;
				/* AVD3D11VADeviceContext: set .device to the OBS device. */
				AVD3D11VADeviceContext *d3d =
					(AVD3D11VADeviceContext *)hwctx->hwctx;
				if (d3d) {
					/* FFmpeg's d3d11va device_free() releases .device,
					   so take our own reference before handing OBS's
					   device over — otherwise that release would drop a
					   reference OBS still owns (the ref is balanced
					   whether init succeeds (freed with the hw ctx) or
					   fails (av_buffer_unref below)). */
					d3d->device = (ID3D11Device *)obs_dev;
					d3d->device->AddRef();
					/* Serialize FFmpeg's use of the (shared, not
					   thread-safe) OBS immediate context with the OBS
					   graphics thread. Must be set before init: the
					   hwcontext only installs its own internal mutex
					   when these are still unset. */
					d3d->lock = zc::zc_d3d11_lock;
					d3d->unlock = zc::zc_d3d11_unlock;
					d3d->lock_ctx = nullptr;
					int ir = av_hwdevice_ctx_init(ref);
					blog(LOG_INFO,
					     "[obs-zcamera] d3d11 hwdevice init=%d "
					     "device=%p feature_level=%x ctx=%p "
					     "video_device=%p video_context=%p",
					     ir, (void *)obs_dev,
					     (unsigned)d3d->device->GetFeatureLevel(),
					     (void *)d3d->device_context,
					     (void *)d3d->video_device,
					     (void *)d3d->video_context);
					if (ir >= 0) {
						f->hw_device_ctx = ref;
						ref = nullptr;
					}
				}
				if (ref)
					av_buffer_unref(&ref);
			}
		}
	}
#endif
	if (!f->hw_device_ctx) {
		/* CUDA/NVDEC: pin FFmpeg to the CUDA device backing the OBS adapter
		   (matched by LUID), so the decode and the interop in
		   hw_decode_cuda_win.cpp run on the GPU OBS renders with. No match
		   means CUDA-D3D11 interop cannot work at all, so fail here and let
		   the caller fall back to another backend. */
		const char *dev_name = NULL;
		char ordinal[16] = {0};
#ifdef _WIN32
		if (hw_type == AV_HWDEVICE_TYPE_CUDA) {
			f->cuda_device = ::zc_hw_cuda_device_for_d3d11(
				(ID3D11Device *)zc::zc_obs_device());
			if (f->cuda_device < 0) {
				blog(LOG_WARNING,
				     "[obs-zcamera] no CUDA device matches the OBS "
				     "adapter; CUDA/NVDEC unavailable");
				delete f;
				return nullptr;
			}
			snprintf(ordinal, sizeof(ordinal), "%d", f->cuda_device);
			dev_name = ordinal;
		}
#endif
		if (av_hwdevice_ctx_create(&f->hw_device_ctx, hw_type, dev_name,
					   NULL, 0) < 0) {
			delete f;
			return nullptr;
		}
#ifdef _WIN32
		if (hw_type == AV_HWDEVICE_TYPE_D3D11VA) {
			/* Only reachable when binding the OBS device failed; the
			   decoder then runs on a private device whose surfaces can
			   never be shared with OBS (zero-copy import will fail). */
			AVD3D11VADeviceContext *d3d =
				(AVD3D11VADeviceContext *)
					((AVHWDeviceContext *)
						 f->hw_device_ctx->data)
						->hwctx;
			blog(LOG_WARNING,
			     "[obs-zcamera] fell back to a private D3D11 device "
			     "(%p, video_context=%p); zero-copy import will fail",
			     (void *)d3d->device, (void *)d3d->video_context);
		}
#endif
	}

f->avcodec = avcodec_find_decoder(zc::zc_to_av_codec(codec));
	if (!f->avcodec) {
		zc::ffmpeg_destroy(&f->base);
		return nullptr;
	}
	f->avctx = avcodec_alloc_context3(f->avcodec);
	if (!f->avctx) {
		zc::ffmpeg_destroy(&f->base);
		return nullptr;
	}
	f->avctx->hw_device_ctx = av_buffer_ref(f->hw_device_ctx);
	f->hw_frame = av_frame_alloc();
	if (!f->hw_frame) {
		zc::ffmpeg_destroy(&f->base);
		return nullptr;
	}

	/* Select the hardware pixel format the decoder should produce, so it uses
	   our hw_frames_ctx instead of falling back to a software pix_fmt. */
	switch (hw_type) {
#ifdef _WIN32
	case AV_HWDEVICE_TYPE_D3D11VA:
		f->hw_pix_fmt = AV_PIX_FMT_D3D11;
		break;
	case AV_HWDEVICE_TYPE_CUDA:
		f->hw_pix_fmt = AV_PIX_FMT_CUDA;
		break;
	case AV_HWDEVICE_TYPE_DXVA2:
		f->hw_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
		break;
	case AV_HWDEVICE_TYPE_QSV:
		f->hw_pix_fmt = AV_PIX_FMT_QSV;
		break;
#endif
#ifdef __APPLE__
	case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
		f->hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
		break;
#endif
	default:
		f->hw_pix_fmt = AV_PIX_FMT_NONE;
		break;
	}
	f->avctx->opaque = f;
	f->avctx->get_format = zc::zc_get_format;

#if defined(_WIN32)
	/* Force the decoder to allocate SHARED D3D11 textures so the surface can
	   be opened in OBS via gs_texture_open_shared (zero-copy). Only valid once
	   the coded dimensions are known (set via set_video_params); otherwise
	   FFmpeg allocates its own (non-shared) pool and import() falls back so we
	   never render from a size-0 frames context. */
	if (hw_type == AV_HWDEVICE_TYPE_D3D11VA && f->width > 0 &&
	    f->height > 0) {
		f->avctx->hw_frames_ctx =
			av_hwframe_ctx_alloc(av_buffer_ref(f->hw_device_ctx));
		if (f->avctx->hw_frames_ctx) {
			AVHWFramesContext *fc =
				(AVHWFramesContext *)f->avctx->hw_frames_ctx->data;
			AVD3D11VAFramesContext *dfc =
				(AVD3D11VAFramesContext *)fc->hwctx;
			fc->format = AV_PIX_FMT_D3D11;
			/* 10-bit streams need P010 surfaces (DXGI_FORMAT_P010);
			   forcing NV12 here would either fail to decode or silently
			   downsample a 10-bit stream to 8-bit. */
			fc->sw_format = f->ten_bit ? AV_PIX_FMT_P010
						   : AV_PIX_FMT_NV12;
			f->hw_sw_fmt = fc->sw_format;
			fc->width = f->width;
			fc->height = f->height;
			dfc->BindFlags = D3D11_BIND_DECODER;
			dfc->MiscFlags = D3D11_RESOURCE_MISC_SHARED;
			{
				int fr = av_hwframe_ctx_init(
					f->avctx->hw_frames_ctx);
				blog(LOG_INFO,
				     "[obs-zcamera] d3d11 hw_frames_ctx init=%d "
				     "(%dx%d)",
				     fr, f->width, f->height);
				if (fr < 0) {
					av_buffer_unref(&f->avctx->hw_frames_ctx);
				}
			}
		}
	}
#endif

	/* Keep the hardware decoder single-threaded. FFmpeg's lock callbacks only
	   cover the device-context calls FFmpeg itself makes, so its frame/slice
	   worker threads would still be several threads inside one decoder driving
	   the one shared immediate context. It also bounds how long the receive
	   thread holds the graphics context (see ZcSspSource::onVideoData). */
	f->avctx->thread_count = 1;

	if (avcodec_open2(f->avctx, f->avcodec, NULL) < 0) {
		zc::ffmpeg_destroy(&f->base);
		return nullptr;
	}
	blog(LOG_INFO,
	     "[obs-zcamera] decoder open: pix_fmt=%s hw_frames_ctx=%s "
	     "threads=%d active_thread_type=%d",
	     av_get_pix_fmt_name(f->avctx->pix_fmt),
	     f->avctx->hw_frames_ctx ? "yes" : "no", f->avctx->thread_count,
	     f->avctx->active_thread_type);

	f->base.decode = zc::ffmpeg_decode;
	f->base.import = zc::ffmpeg_import;
	f->base.set_video_params = zc::ffmpeg_set_params;
	f->base.release = zc::ffmpeg_release;
	f->base.destroy = zc::ffmpeg_destroy;
	return &f->base;
}
