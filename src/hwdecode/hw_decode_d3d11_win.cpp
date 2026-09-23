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

/* Windows D3D11VA shared-texture zero-copy import.

   The D3D11VA decoder hands back one element of an NV12 *texture array*
   (avf->data[1] is the array slice). Neither of those can be opened by OBS
   directly:

     - gs_texture_open_shared() opens a non-array 2D view, and
     - libobs-d3d11's ConvertDXGITextureFormat() has no DXGI_FORMAT_NV12 case,
       so the SRV would be created with DXGI_FORMAT_UNKNOWN and fail.

   So the surface is first blitted by the D3D11 VideoProcessor into a plain
   non-array BGRA texture created with D3D11_RESOURCE_MISC_SHARED. BGRA is a
   format libobs-d3d11 does support, and a shared non-array texture is exactly
   what gs_texture_open_shared() expects. The blit happens on the GPU: no
   av_hwframe_transfer_data, no CPU round trip.

   Note the request for D3D11_RESOURCE_MISC_SHARED on
   AVD3D11VAFramesContext (hw_decode_ffmpeg.cpp) is NOT applied to the
   decoder's own pool — the surfaces come back with MiscFlags == 0 — so the
   decoder's texture itself can never be shared. Blitting into our own shared
   texture sidesteps that entirely.

   All of this runs on the graphics thread, inside obs_enter_graphics(). */

#include "hw_decode_ffmpeg.h"
#include "hw_decode.h"

#include <obs-module.h>
#include <obs.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <d3d11.h>
#include <dxgi1_2.h>

#include <new>

/* At global scope: hw_decode_ffmpeg.h forward-declares it there (inside
   extern "C"), so the definition must not live in namespace zc. */
/* Cached per decoder: the video processor, its enumerator, the shared output
   texture and the input/output views. Created once on the first import and
   reused for every frame. */
struct zc_hw_d3d11_import {
	ID3D11Device *device = NULL;          /* AddRef'd by GetDevice()      */
	ID3D11DeviceContext *context = NULL;  /* AddRef'd by GetImmediateContext */
	ID3D11VideoDevice *vdev = NULL;
	ID3D11VideoContext *vctx = NULL;
	ID3D11VideoProcessorEnumerator *enumr = NULL;
	ID3D11VideoProcessor *vp = NULL;
	ID3D11Texture2D *out_tex = NULL;
	ID3D11VideoProcessorOutputView *out_view = NULL;
	HANDLE shared_handle = NULL;
	uint32_t width = 0;
	uint32_t height = 0;
	/* Set when the processor path proved unsupported for this decoder, so the
	   setup is attempted once rather than at frame rate. */
	bool unavailable = false;
};

/* The D3D11VA decoder always produces NV12 (see AVD3D11VAFramesContext.sw_format
   in hw_decode_ffmpeg.cpp); the output is BGRA because that is what
   libobs-d3d11 can create an SRV for. */
static const DXGI_FORMAT kInputFormat = DXGI_FORMAT_NV12;
static const DXGI_FORMAT kOutputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

static void vp_clear(struct zc_hw_d3d11_import *imp)
{
	if (!imp)
		return;
	if (imp->out_view) {
		imp->out_view->Release();
		imp->out_view = NULL;
	}
	if (imp->out_tex) {
		imp->out_tex->Release();
		imp->out_tex = NULL;
	}
	if (imp->vp) {
		imp->vp->Release();
		imp->vp = NULL;
	}
	if (imp->enumr) {
		imp->enumr->Release();
		imp->enumr = NULL;
	}
	if (imp->vctx) {
		imp->vctx->Release();
		imp->vctx = NULL;
	}
	if (imp->vdev) {
		imp->vdev->Release();
		imp->vdev = NULL;
	}
	if (imp->context) {
		imp->context->Release();
		imp->context = NULL;
	}
	if (imp->device) {
		imp->device->Release();
		imp->device = NULL;
	}
	imp->shared_handle = NULL;
	imp->width = 0;
	imp->height = 0;
}

/* Build the video processor + the shared BGRA output texture. Returns false
   and marks the cache unavailable when the GPU cannot do the conversion. */
static bool vp_setup(struct zc_hw_d3d11_import *imp, ID3D11Texture2D *in_tex,
		     uint32_t width, uint32_t height)
{
	in_tex->GetDevice(&imp->device);
	if (!imp->device) {
		blog(LOG_WARNING, "[obs-zcamera] d3d11 vp: no device on texture");
		return false;
	}
	imp->device->GetImmediateContext(&imp->context);
	if (!imp->context) {
		blog(LOG_WARNING, "[obs-zcamera] d3d11 vp: no immediate context");
		return false;
	}
	if (FAILED(imp->device->QueryInterface(
			__uuidof(ID3D11VideoDevice), (void **)&imp->vdev)) ||
	    FAILED(imp->context->QueryInterface(
			__uuidof(ID3D11VideoContext), (void **)&imp->vctx))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: device has no video processor "
		     "interface");
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
	cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	cd.InputWidth = width;
	cd.InputHeight = height;
	cd.OutputWidth = width;
	cd.OutputHeight = height;
	cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(imp->vdev->CreateVideoProcessorEnumerator(&cd, &imp->enumr))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: CreateVideoProcessorEnumerator "
		     "failed (%ux%u)",
		     width, height);
		return false;
	}

	UINT fmt_flags = 0;
	if (FAILED(imp->enumr->CheckVideoProcessorFormat(kInputFormat,
							 &fmt_flags)) ||
	    !(fmt_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: NV12 is not a supported video "
		     "processor input on this GPU");
		return false;
	}
	if (FAILED(imp->vdev->CreateVideoProcessor(imp->enumr, 0, &imp->vp))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: CreateVideoProcessor failed");
		return false;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = kOutputFormat;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	if (FAILED(imp->device->CreateTexture2D(&td, NULL, &imp->out_tex))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: shared BGRA texture creation "
		     "failed (%ux%u)",
		     width, height);
		return false;
	}

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
	ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	ovd.Texture2D.MipSlice = 0;
	if (FAILED(imp->vdev->CreateVideoProcessorOutputView(
			imp->out_tex, imp->enumr, &ovd, &imp->out_view))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: CreateVideoProcessorOutputView "
		     "failed");
		return false;
	}

	IDXGIResource *res = NULL;
	if (FAILED(imp->out_tex->QueryInterface(__uuidof(IDXGIResource),
						(void **)&res))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: output texture is not an "
		     "IDXGIResource");
		return false;
	}
	const HRESULT hr = res->GetSharedHandle(&imp->shared_handle);
	res->Release();
	if (FAILED(hr) || !imp->shared_handle) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 vp: no shared handle on the BGRA "
		     "texture");
		return false;
	}

	imp->width = width;
	imp->height = height;
	blog(LOG_INFO,
	     "[obs-zcamera] d3d11 video processor ready: NV12 slice -> shared "
	     "BGRA %ux%u (zero-copy import path)",
	     width, height);
	return true;
}

/* Declared in hw_decode_ffmpeg.h inside `extern "C"`; called by the decoder's
   destroy() on the graphics thread. */
void zc_hw_d3d11_import_free(struct zc_hw_d3d11_import *imp)
{
	if (!imp)
		return;
	vp_clear(imp);
	delete imp;
}

namespace zc {

gs_texture_t *zc_hw_import_d3d11_slice(struct zc_hw_d3d11_import **slot,
				       AVFrame *avf, int slice)
{
	if (!slot) {
		blog(LOG_WARNING, "[obs-zcamera] d3d11 import: no cache slot");
		return NULL;
	}
	if (!avf) {
		blog(LOG_WARNING, "[obs-zcamera] d3d11 import: null AVFrame");
		return NULL;
	}
	if (*slot && (*slot)->unavailable)
		return NULL;

	ID3D11Texture2D *in_tex = (ID3D11Texture2D *)avf->data[0];
	if (!in_tex) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: no texture in data[0]");
		return NULL;
	}

	/* A null slot must be allocated before any early-out below can latch it. */
	if (!*slot) {
		*slot = new (std::nothrow) zc_hw_d3d11_import;
		if (!*slot)
			return NULL;
	}
	struct zc_hw_d3d11_import *imp = *slot;

	D3D11_TEXTURE2D_DESC in_desc = {};
	in_tex->GetDesc(&in_desc);

	/* The decoder's surface must currently be NV12 (the only format this
	   conversion handles); otherwise let the caller fall back. */
	if (in_desc.Format != kInputFormat) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: decoder surface is DXGI "
		     "format %d, expected NV12; using software decode",
		     (int)in_desc.Format);
		imp->unavailable = true;
		return NULL;
	}
	if (slice < 0 || (UINT)slice >= in_desc.ArraySize) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: slice %d out of range "
		     "(ArraySize=%u)",
		     slice, in_desc.ArraySize);
		return NULL;
	}

	if (!imp->vp) {
		if (!vp_setup(imp, in_tex, (uint32_t)in_desc.Width,
			      (uint32_t)in_desc.Height)) {
			imp->unavailable = true;
			vp_clear(imp);
			return NULL;
		}
	} else if ((uint32_t)in_desc.Width != imp->width ||
		   (uint32_t)in_desc.Height != imp->height) {
		/* Resolution change: the cached processor and output texture are
		   sized for the old dimensions. */
		vp_clear(imp);
		if (!vp_setup(imp, in_tex, (uint32_t)in_desc.Width,
			      (uint32_t)in_desc.Height)) {
			imp->unavailable = true;
			vp_clear(imp);
			return NULL;
		}
	}

	/* One input view per array slice: this is what makes the decoder's
	   texture *array* usable — OBS's own import could only ever see a
	   non-array view. */
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
	ivd.FourCC = 0;
	ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	ivd.Texture2D.MipSlice = 0;
	ivd.Texture2D.ArraySlice = (UINT)slice;

	ID3D11VideoProcessorInputView *in_view = NULL;
	if (FAILED(imp->vdev->CreateVideoProcessorInputView(
			in_tex, imp->enumr, &ivd, &in_view))) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: input view for slice %d "
		     "failed",
		     slice);
		return NULL;
	}

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.OutputIndex = 0;
	stream.InputFrameOrField = 0;
	stream.PastFrames = 0;
	stream.FutureFrames = 0;
	stream.pInputSurface = in_view;

	const HRESULT hr =
		imp->vctx->VideoProcessorBlt(imp->vp, imp->out_view, 0, 1,
					     &stream);
	in_view->Release();
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: VideoProcessorBlt failed "
		     "(0x%08x)",
		     (unsigned)hr);
		return NULL;
	}

	/* Open the shared BGRA texture in OBS. gs_texture_open_shared takes the
	   low 32 bits of the legacy shared handle. */
	gs_texture_t *gstex =
		gs_texture_open_shared((uint32_t)(uintptr_t)imp->shared_handle);
	if (!gstex) {
		blog(LOG_WARNING,
		     "[obs-zcamera] d3d11 import: gs_texture_open_shared failed "
		     "(handle %p)",
		     (void *)imp->shared_handle);
		return NULL;
	}
	return gstex;
}

} // namespace zc