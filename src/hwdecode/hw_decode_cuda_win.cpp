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

/* Windows CUDA/NVDEC zero-copy import.

   FFmpeg's CUDA hwaccel (NVDEC) hands back AV_PIX_FMT_CUDA frames whose planes
   live in CUDA device memory: avf->data[0]/data[1] are CUdeviceptr for the NV12
   luma and chroma planes, avf->linesize[] the (pitch-aligned) strides. Nothing
   there is sampleable by OBS, and CUDA frames cannot be mapped to D3D11
   (av_hwframe_map cannot cross the two APIs), so the frame has to be moved onto
   a D3D11 texture OBS can open.

   Two measured facts shape this implementation (see
   tests/pipeline_test.cpp --backend cuda):

     1. cuGraphicsD3D11RegisterResource() does accept a D3D11 *NV12* texture
        (created with D3D11_BIND_DECODER), but CUDA then exposes ONLY its luma
        plane: the mapped CUarray is width x height, 1 x UNORM8, subresource
        index 1 fails with CUDA_ERROR_INVALID_VALUE and the luma-only array
        rejects dstY >= height. So "let CUDA write the decoded NV12 straight into
        a D3D11 NV12 texture" is not possible.
     2. CUDA's classic D3D11 interop formats are exactly the two NVDEC planes,
        so each plane can be registered and filled on its own, on the GPU:
        NV12 luma -> R8_UNORM, NV12 chroma -> R8G8_UNORM, both with cuMemcpy2D
        (device memory -> CUDA array). No CPU round trip.

   So an import is:

        cuMemcpy2D  luma   -> R8_UNORM   texture
        cuMemcpy2D  chroma -> R8G8_UNORM texture
        full-screen YUV->RGB pass -> shared B8G8R8A8 texture
        gs_texture_open_shared()  (identical to the D3D11VA path)

   The BGRA texture is created with D3D11_RESOURCE_MISC_SHARED and opened by OBS
   exactly like the D3D11VA one, so videoRender() needs no backend-specific code.

   nvcuda.dll's driver API is loaded lazily: the plugin must keep working on
   machines without an NVIDIA GPU and the CUDA toolkit is not a build
   dependency.

   The copies read the plane pointers FFmpeg's decoder wrote, from a context
   this file creates itself on the same device (unified addressing makes the
   pointers valid in both). Owning the context keeps cuda.h out of the build and
   keeps the graphics thread from sharing a context with the decode thread.

   All of this runs on the graphics thread, inside obs_enter_graphics(). */

#include "hw_decode_ffmpeg.h"
#include "hw_decode.h"

#include <obs-module.h>
#include <obs.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>
#include <mutex>
#include <new>

/* ================================================================== */
/* Minimal CUDA driver API (nvcuda.dll), declared here on purpose so  */
/* the toolchain needs no CUDA headers or import library.             */
/* ================================================================== */

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUarray_st *CUarray;
typedef struct CUgraphicsResource_st *CUgraphicsResource;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;

#define ZC_CUDA_SUCCESS 0

enum {
	CU_MEMORYTYPE_HOST = 0x01,
	CU_MEMORYTYPE_DEVICE = 0x02,
	CU_MEMORYTYPE_ARRAY = 0x03,
};

/* Same layout as CUDA_MEMCPY2D (enums are int on Windows). */
struct zc_cuda_memcpy2d {
	size_t srcXInBytes;
	size_t srcY;
	int srcMemoryType;
	const void *srcHost;
	CUdeviceptr srcDevice;
	CUarray srcArray;
	size_t srcPitch;
	size_t dstXInBytes;
	size_t dstY;
	int dstMemoryType;
	void *dstHost;
	CUdeviceptr dstDevice;
	CUarray dstArray;
	size_t dstPitch;
	size_t WidthInBytes;
	size_t Height;
};

typedef CUresult(__stdcall *zc_cuInit_t)(unsigned int);
typedef CUresult(__stdcall *zc_cuDeviceGetCount_t)(int *);
typedef CUresult(__stdcall *zc_cuDeviceGet_t)(CUdevice *, int);
typedef CUresult(__stdcall *zc_cuDeviceGetLuid_t)(char *, unsigned int *, CUdevice);
typedef CUresult(__stdcall *zc_cuCtxCreate_t)(CUcontext *, unsigned int, CUdevice);
typedef CUresult(__stdcall *zc_cuCtxDestroy_t)(CUcontext);
typedef CUresult(__stdcall *zc_cuCtxPushCurrent_t)(CUcontext);
typedef CUresult(__stdcall *zc_cuCtxPopCurrent_t)(CUcontext *);
typedef CUresult(__stdcall *zc_cuCtxSynchronize_t)(void);
typedef CUresult(__stdcall *zc_cuGraphicsD3D11RegisterResource_t)(
	CUgraphicsResource *, ID3D11Resource *, unsigned int);
typedef CUresult(__stdcall *zc_cuGraphicsMapResources_t)(unsigned int,
							 CUgraphicsResource *,
							 CUstream);
typedef CUresult(__stdcall *zc_cuGraphicsUnmapResources_t)(unsigned int,
							   CUgraphicsResource *,
							   CUstream);
typedef CUresult(__stdcall *zc_cuGraphicsSubResourceGetMappedArray_t)(
	CUarray *, CUgraphicsResource, unsigned int, unsigned int);
typedef CUresult(__stdcall *zc_cuMemcpy2D_t)(const zc_cuda_memcpy2d *);
typedef CUresult(__stdcall *zc_cuGraphicsUnregisterResource_t)(CUgraphicsResource);
typedef CUresult(__stdcall *zc_cuGetErrorName_t)(CUresult, const char **);

struct zc_cuda_api {
	HMODULE mod = nullptr;
	bool loaded = false;

	zc_cuInit_t cuInit = nullptr;
	zc_cuDeviceGetCount_t cuDeviceGetCount = nullptr;
	zc_cuDeviceGet_t cuDeviceGet = nullptr;
	zc_cuDeviceGetLuid_t cuDeviceGetLuid = nullptr;
	zc_cuCtxCreate_t cuCtxCreate = nullptr;
	zc_cuCtxDestroy_t cuCtxDestroy = nullptr;
	zc_cuCtxPushCurrent_t cuCtxPushCurrent = nullptr;
	zc_cuCtxPopCurrent_t cuCtxPopCurrent = nullptr;
	zc_cuCtxSynchronize_t cuCtxSynchronize = nullptr;
	zc_cuGraphicsD3D11RegisterResource_t cuGraphicsD3D11RegisterResource = nullptr;
	zc_cuGraphicsMapResources_t cuGraphicsMapResources = nullptr;
	zc_cuGraphicsUnmapResources_t cuGraphicsUnmapResources = nullptr;
	zc_cuGraphicsSubResourceGetMappedArray_t
		cuGraphicsSubResourceGetMappedArray = nullptr;
	zc_cuMemcpy2D_t cuMemcpy2D = nullptr;
	zc_cuGraphicsUnregisterResource_t cuGraphicsUnregisterResource = nullptr;
	zc_cuGetErrorName_t cuGetErrorName = nullptr;

	const char *err(CUresult r) const
	{
		const char *name = nullptr;
		if (cuGetErrorName && cuGetErrorName(r, &name) == ZC_CUDA_SUCCESS &&
		    name)
			return name;
		return "?";
	}
};

static const zc_cuda_api *zc_cuda_api_get(void)
{
	static std::once_flag once;
	static zc_cuda_api api;

	std::call_once(once, []() {
		api.mod = LoadLibraryA("nvcuda.dll");
		if (!api.mod) {
			blog(LOG_INFO,
			     "[obs-zcamera] cuda import: nvcuda.dll not present "
			     "(no NVIDIA driver)");
			return;
		}
		struct {
			const char *names[2];
			void **slot;
		} entries[] = {
			{{"cuInit"}, (void **)&api.cuInit},
			{{"cuDeviceGetCount"}, (void **)&api.cuDeviceGetCount},
			{{"cuDeviceGet"}, (void **)&api.cuDeviceGet},
			{{"cuDeviceGetLuid"}, (void **)&api.cuDeviceGetLuid},
			{{"cuCtxCreate_v2", "cuCtxCreate"}, (void **)&api.cuCtxCreate},
			{{"cuCtxDestroy_v2", "cuCtxDestroy"},
			 (void **)&api.cuCtxDestroy},
			{{"cuCtxPushCurrent_v2", "cuCtxPushCurrent"},
			 (void **)&api.cuCtxPushCurrent},
			{{"cuCtxPopCurrent_v2", "cuCtxPopCurrent"},
			 (void **)&api.cuCtxPopCurrent},
			{{"cuCtxSynchronize", "cuCtxSynchronize_v2"},
			 (void **)&api.cuCtxSynchronize},
			{{"cuGraphicsD3D11RegisterResource"},
			 (void **)&api.cuGraphicsD3D11RegisterResource},
			{{"cuGraphicsMapResources"},
			 (void **)&api.cuGraphicsMapResources},
			{{"cuGraphicsUnmapResources"},
			 (void **)&api.cuGraphicsUnmapResources},
			{{"cuGraphicsSubResourceGetMappedArray"},
			 (void **)&api.cuGraphicsSubResourceGetMappedArray},
			{{"cuMemcpy2D_v2", "cuMemcpy2D"}, (void **)&api.cuMemcpy2D},
			{{"cuGraphicsUnregisterResource"},
			 (void **)&api.cuGraphicsUnregisterResource},
		};
		for (auto &e : entries) {
			*e.slot = nullptr;
			for (const char *nm : e.names) {
				if (!nm)
					break;
				*e.slot = (void *)GetProcAddress(api.mod, nm);
				if (*e.slot)
					break;
			}
			if (!*e.slot) {
				blog(LOG_WARNING,
				     "[obs-zcamera] cuda import: nvcuda.dll is "
				     "missing entry point %s",
				     e.names[0]);
				return;
			}
		}
		/* Optional: only used for readable error names. */
		api.cuGetErrorName = (zc_cuGetErrorName_t)GetProcAddress(
			api.mod, "cuGetErrorName");
		api.loaded = true;
	});

	return api.loaded ? &api : nullptr;
}

/* ================================================================== */
/* per-decoder import state                                            */
/* ================================================================== */

/* Structs here (as in hw_decode_d3d11_win.cpp) because the importer has no
   C++ linkage contract with the decoder. */
struct zc_hw_cuda_import {
	ID3D11Device *device = nullptr;        /* AddRef'd */
	ID3D11DeviceContext *context = nullptr; /* AddRef'd */
	ID3D11Texture2D *y_tex = nullptr;      /* R8_UNORM, coded size     */
	ID3D11Texture2D *uv_tex = nullptr;     /* R8G8_UNORM, half size    */
	ID3D11ShaderResourceView *y_srv = nullptr;
	ID3D11ShaderResourceView *uv_srv = nullptr;
	ID3D11Texture2D *out_tex = nullptr;    /* shared BGRA for OBS      */
	ID3D11RenderTargetView *out_rtv = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11SamplerState *samp = nullptr;
	ID3D11Buffer *params = nullptr;        /* colorspace/range flags   */
	CUgraphicsResource y_res = nullptr;
	CUgraphicsResource uv_res = nullptr;
	/* Our own context on the adapter OBS renders on. Created here rather than
	   borrowed from FFmpeg's device: the importer then owns the lifetime, no
	   CUDA header (cuda.h) becomes a build dependency, and the graphics thread
	   never shares a context with the decoder thread. */
	CUcontext cuda_ctx = nullptr;
	HANDLE shared_handle = nullptr;
	uint32_t width = 0, height = 0;
	/* The whole import proved impossible for this decoder: keep it latched so
	   the (failing) setup is not retried at frame rate. */
	bool unavailable = false;
};

/* Full-screen triangle (no vertex buffer: SV_VertexID) + NV12 -> BGRA. */
static const char *kCudaConvertShader = R"(
struct VSOut {
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
	VSOut o;
	float2 p = float2((id << 1) & 2, id & 2);
	o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
	o.uv = p;
	return o;
}

Texture2D    tex_y  : register(t0);
Texture2D    tex_uv : register(t1);
SamplerState samp   : register(s0);
cbuffer Params : register(b0) { float4 params; }; /* x: BT.709, y: full range */

float4 PSMain(VSOut i) : SV_Target
{
	float y = tex_y.Sample(samp, i.uv).r;
	float2 uv = tex_uv.Sample(samp, i.uv).rg;

	if (params.y < 0.5) {
		/* limited ("studio") range */
		y = (y - 16.0 / 255.0) * (255.0 / 219.0);
		uv = (uv - 128.0 / 255.0) * (255.0 / 224.0);
	} else {
		uv = uv - 128.0 / 255.0;
	}

	float3 rgb;
	if (params.x > 0.5) { /* BT.709 */
		rgb = float3(y + 1.5748 * uv.y,
			     y - 0.1873 * uv.x - 0.4681 * uv.y,
			     y + 1.8556 * uv.x);
	} else { /* BT.601 */
		rgb = float3(y + 1.402 * uv.y,
			     y - 0.3441 * uv.x - 0.7141 * uv.y,
			     y + 1.772 * uv.x);
	}
	return float4(saturate(rgb), 1.0);
}
)";

static void cuda_release_d3d(struct zc_hw_cuda_import *imp)
{
#define ZC_CUDA_REL(x)                     \
	if (imp->x) {                      \
		imp->x->Release();         \
		imp->x = nullptr;          \
	}
	ZC_CUDA_REL(params);
	ZC_CUDA_REL(samp);
	ZC_CUDA_REL(ps);
	ZC_CUDA_REL(vs);
	ZC_CUDA_REL(out_rtv);
	ZC_CUDA_REL(out_tex);
	ZC_CUDA_REL(uv_srv);
	ZC_CUDA_REL(y_srv);
	ZC_CUDA_REL(uv_tex);
	ZC_CUDA_REL(y_tex);
	ZC_CUDA_REL(context);
	ZC_CUDA_REL(device);
#undef ZC_CUDA_REL
	imp->shared_handle = nullptr;
	imp->width = 0;
	imp->height = 0;
}

/* Unregister the CUDA views (needs the CUDA context current), destroy that
   context and release everything else. Safe to call repeatedly. */
static void cuda_clear(struct zc_hw_cuda_import *imp)
{
	if (!imp)
		return;

	const zc_cuda_api *api = zc_cuda_api_get();
	if (api && imp->cuda_ctx) {
		CUcontext prev = nullptr;
		api->cuCtxPushCurrent(imp->cuda_ctx);
		if (imp->y_res) {
			api->cuGraphicsUnregisterResource(imp->y_res);
			imp->y_res = nullptr;
		}
		if (imp->uv_res) {
			api->cuGraphicsUnregisterResource(imp->uv_res);
			imp->uv_res = nullptr;
		}
		api->cuCtxPopCurrent(&prev);
		api->cuCtxDestroy(imp->cuda_ctx);
		imp->cuda_ctx = nullptr;
	}
	cuda_release_d3d(imp);
}

static bool cuda_compile_shaders(struct zc_hw_cuda_import *imp)
{
	const size_t len = strlen(kCudaConvertShader);

	ID3DBlob *vs_blob = nullptr, *ps_blob = nullptr, *errors = nullptr;
	HRESULT hr = D3DCompile(kCudaConvertShader, len, "zc_cuda_convert", nullptr,
				nullptr, "VSMain", "vs_5_0", 0, 0, &vs_blob,
				&errors);
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: vertex shader compile failed: %s",
		     errors ? (const char *)errors->GetBufferPointer() : "(no msg)");
		if (errors)
			errors->Release();
		return false;
	}
	if (errors) {
		errors->Release();
		errors = nullptr;
	}
	hr = D3DCompile(kCudaConvertShader, len, "zc_cuda_convert", nullptr, nullptr,
			"PSMain", "ps_5_0", 0, 0, &ps_blob, &errors);
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: pixel shader compile failed: %s",
		     errors ? (const char *)errors->GetBufferPointer() : "(no msg)");
		vs_blob->Release();
		if (errors)
			errors->Release();
		return false;
	}
	if (errors)
		errors->Release();

	bool ok = SUCCEEDED(imp->device->CreateVertexShader(
			    vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
			    nullptr, &imp->vs)) &&
		  SUCCEEDED(imp->device->CreatePixelShader(
			    ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
			    nullptr, &imp->ps));
	vs_blob->Release();
	ps_blob->Release();
	if (!ok)
		blog(LOG_WARNING, "[obs-zcamera] cuda import: shader creation failed");
	return ok;
}

/* Create the plane textures, the shared BGRA texture, the conversion pass and
   register the planes with CUDA. Returns false on the first failure. */
static bool cuda_setup(struct zc_hw_cuda_import *imp, const zc_cuda_api *api,
		       ID3D11Device *device, int cuda_device, uint32_t width,
		       uint32_t height)
{
	imp->device = device;
	imp->device->AddRef();
	imp->device->GetImmediateContext(&imp->context);
	if (!imp->context) {
		blog(LOG_WARNING, "[obs-zcamera] cuda import: no immediate context");
		return false;
	}

	/* A context on the same device as the D3D11 device is what makes
	   cuGraphicsD3D11RegisterResource legal. */
	{
		CUdevice dev = 0;
		CUresult r = api->cuInit(0);
		if (r == ZC_CUDA_SUCCESS)
			r = api->cuDeviceGet(&dev, cuda_device);
		if (r == ZC_CUDA_SUCCESS)
			r = api->cuCtxCreate(&imp->cuda_ctx, 0 /* SCHED_AUTO */, dev);
		if (r != ZC_CUDA_SUCCESS) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: no CUDA context on device %d "
			     "(%s)",
			     cuda_device, api->err(r));
			imp->cuda_ctx = nullptr;
			return false;
		}
	}

	/* Plane textures: plain 2D textures with an SRV, which is all CUDA's
	   D3D11 interop needs and all the conversion pass samples. */
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width;
		td.Height = height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(imp->device->CreateTexture2D(&td, nullptr, &imp->y_tex))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: luma texture creation failed");
			return false;
		}
		td.Width = width / 2;
		td.Height = height / 2;
		td.Format = DXGI_FORMAT_R8G8_UNORM;
		if (FAILED(imp->device->CreateTexture2D(&td, nullptr, &imp->uv_tex))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: chroma texture creation "
			     "failed");
			return false;
		}
		if (FAILED(imp->device->CreateShaderResourceView(imp->y_tex, nullptr,
								 &imp->y_srv)) ||
		    FAILED(imp->device->CreateShaderResourceView(imp->uv_tex, nullptr,
								 &imp->uv_srv))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: plane views creation failed");
			return false;
		}
	}

	/* The BGRA texture OBS opens: same recipe the D3D11VA video-processor path
	   uses (shareable, render target + shader resource, BGRA is a format
	   libobs-d3d11 can create an SRV for). */
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width;
		td.Height = height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		if (FAILED(imp->device->CreateTexture2D(&td, nullptr, &imp->out_tex)) ||
		    FAILED(imp->device->CreateRenderTargetView(imp->out_tex, nullptr,
							       &imp->out_rtv))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: shared BGRA texture creation "
			     "failed (%ux%u)",
			     width, height);
			return false;
		}
		IDXGIResource *res = nullptr;
		if (FAILED(imp->out_tex->QueryInterface(__uuidof(IDXGIResource),
							(void **)&res))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: output texture is not an "
			     "IDXGIResource");
			return false;
		}
		const HRESULT hr = res->GetSharedHandle(&imp->shared_handle);
		res->Release();
		if (FAILED(hr) || !imp->shared_handle) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: no shared handle on the BGRA "
			     "texture");
			return false;
		}
	}

	if (!cuda_compile_shaders(imp))
		return false;

	{
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(imp->device->CreateSamplerState(&sd, &imp->samp))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: sampler creation failed");
			return false;
		}

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 16; /* float4 params */
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(imp->device->CreateBuffer(&bd, nullptr, &imp->params))) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: constant buffer creation "
			     "failed");
			return false;
		}
	}

	/* Hand the planes to CUDA on our context. */
	CUcontext prev = nullptr;
	api->cuCtxPushCurrent(imp->cuda_ctx);
	CUresult r = api->cuGraphicsD3D11RegisterResource(
		&imp->y_res, (ID3D11Resource *)imp->y_tex, 0);
	if (r == ZC_CUDA_SUCCESS)
		r = api->cuGraphicsD3D11RegisterResource(
			&imp->uv_res, (ID3D11Resource *)imp->uv_tex, 0);
	api->cuCtxPopCurrent(&prev);
	if (r != ZC_CUDA_SUCCESS) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: D3D11 registration failed (%s); "
		     "the D3D11 device must be the one CUDA is on",
		     api->err(r));
		return false;
	}

	imp->width = width;
	imp->height = height;
	blog(LOG_INFO,
	     "[obs-zcamera] cuda import ready: NVDEC NV12 -> R8/R8G8 -> shared BGRA "
	     "%ux%u (zero-copy)",
	     width, height);
	return true;
}

/* NVDEC planes -> the registered textures, entirely on the GPU. */
static bool cuda_copy_planes(const zc_cuda_api *api, struct zc_hw_cuda_import *imp,
			     AVFrame *avf)
{
	CUcontext prev = nullptr;

	/* Every CUDA entry point below acts on the context that is current on this
	   thread, and cuda_setup() popped this importer's context again after
	   registering the planes — so it has to be pushed back here, and the result
	   checked: without a current context the map fails with
	   CUDA_ERROR_INVALID_CONTEXT (which the source would read as "this
	   connection cannot be zero-copy imported"). */
	const CUresult rp = api->cuCtxPushCurrent(imp->cuda_ctx);
	if (rp != ZC_CUDA_SUCCESS) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: no current CUDA context (%s)",
		     api->err(rp));
		return false;
	}

	bool ok = false;
	CUgraphicsResource res[2] = {imp->y_res, imp->uv_res};
	CUresult r = api->cuGraphicsMapResources(2, res, nullptr);
	if (r != ZC_CUDA_SUCCESS) {
		blog(LOG_WARNING, "[obs-zcamera] cuda import: map failed (%s)",
		     api->err(r));
	} else {
		CUarray y_arr = nullptr, uv_arr = nullptr;
		const CUresult r_y = api->cuGraphicsSubResourceGetMappedArray(
			&y_arr, imp->y_res, 0, 0);
		const CUresult r_uv = api->cuGraphicsSubResourceGetMappedArray(
			&uv_arr, imp->uv_res, 0, 0);
		if (r_y != ZC_CUDA_SUCCESS || r_uv != ZC_CUDA_SUCCESS || !y_arr ||
		    !uv_arr) {
			blog(LOG_WARNING,
			     "[obs-zcamera] cuda import: mapped array unavailable "
			     "(%s / %s)",
			     api->err(r_y), api->err(r_uv));
		} else {
			zc_cuda_memcpy2d cp = {};
			cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
			cp.srcDevice = (CUdeviceptr)(uintptr_t)avf->data[0];
			cp.srcPitch = (size_t)avf->linesize[0];
			cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
			cp.dstArray = y_arr;
			cp.WidthInBytes = imp->width;
			cp.Height = imp->height;
			r = api->cuMemcpy2D(&cp);
			if (r == ZC_CUDA_SUCCESS) {
				/* The chroma plane is interleaved UV: width bytes per
				   row is exactly width/2 pairs, i.e. one R8G8 row. */
				zc_cuda_memcpy2d cc = {};
				cc.srcMemoryType = CU_MEMORYTYPE_DEVICE;
				cc.srcDevice = (CUdeviceptr)(uintptr_t)avf->data[1];
				cc.srcPitch = (size_t)avf->linesize[1];
				cc.dstMemoryType = CU_MEMORYTYPE_ARRAY;
				cc.dstArray = uv_arr;
				cc.WidthInBytes = imp->width;
				cc.Height = imp->height / 2;
				r = api->cuMemcpy2D(&cc);
			}
			if (r != ZC_CUDA_SUCCESS) {
				blog(LOG_WARNING,
				     "[obs-zcamera] cuda import: plane copy failed "
				     "(%s)",
				     api->err(r));
			} else {
				/* CUDA and D3D11 execute independently: without this
				   wait, the draw in cuda_render() reads the plane
				   textures while the copies are still in flight, which
				   tears the picture. */
				const CUresult rs = api->cuCtxSynchronize();
				if (rs != ZC_CUDA_SUCCESS)
					blog(LOG_WARNING,
					     "[obs-zcamera] cuda import: plane copy did "
					     "not complete (%s)",
					     api->err(rs));
				else
					ok = true;
			}
		}
		api->cuGraphicsUnmapResources(2, res, nullptr);
	}

	api->cuCtxPopCurrent(&prev);
	return ok;
}

/* YUV -> BGRA into the shared texture. The colorspace/range come from the
   stream when FFmpeg reported them; unspecified legacy SD is BT.601. */
static bool cuda_render(const AVFrame *avf, struct zc_hw_cuda_import *imp)
{
	float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	if (avf->colorspace == AVCOL_SPC_BT709 ||
	    avf->colorspace == AVCOL_SPC_BT2020_NCL ||
	    avf->colorspace == AVCOL_SPC_BT2020_CL)
		params[0] = 1.0f;
	if (avf->color_range == AVCOL_RANGE_JPEG)
		params[1] = 1.0f;

	imp->context->UpdateSubresource(imp->params, 0, nullptr, params, 0, 0);

	ID3D11RenderTargetView *rtv = imp->out_rtv;
	imp->context->OMSetRenderTargets(1, &rtv, nullptr);
	D3D11_VIEWPORT vp = {};
	vp.Width = (float)imp->width;
	vp.Height = (float)imp->height;
	vp.MaxDepth = 1.0f;
	imp->context->RSSetViewports(1, &vp);
	imp->context->IASetInputLayout(nullptr);
	imp->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	imp->context->VSSetShader(imp->vs, nullptr, 0);
	imp->context->PSSetShader(imp->ps, nullptr, 0);
	ID3D11ShaderResourceView *srvs[2] = {imp->y_srv, imp->uv_srv};
	imp->context->PSSetShaderResources(0, 2, srvs);
	imp->context->PSSetSamplers(0, 1, &imp->samp);
	imp->context->PSSetConstantBuffers(0, 1, &imp->params);
	imp->context->Draw(3, 0);

	/* Unbind before CUDA writes the planes again next frame. */
	ID3D11ShaderResourceView *unbind[2] = {nullptr, nullptr};
	imp->context->PSSetShaderResources(0, 2, unbind);
	imp->context->OMSetRenderTargets(0, nullptr, nullptr);
	return true;
}

/* ================================================================== */
/* interface used by the FFmpeg decoder                                */
/* ================================================================== */

/* Which CUDA device backs the D3D11 device OBS renders on? Matching by adapter
   LUID keeps CUDA-D3D11 interop on a laptop's dGPU instead of its iGPU.
   Returns the CUDA ordinal, or -1 when it cannot be determined. */
int zc_hw_cuda_device_for_d3d11(struct ID3D11Device *device)
{
	if (!device)
		return -1;

	IDXGIDevice *dxgi = nullptr;
	if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) ||
	    !dxgi)
		return -1;

	int ordinal = -1;
	IDXGIAdapter *adapter = nullptr;
	if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
		DXGI_ADAPTER_DESC ad = {};
		if (SUCCEEDED(adapter->GetDesc(&ad))) {
			const zc_cuda_api *api = zc_cuda_api_get();
			int count = 0;
			if (api && api->cuInit(0) == ZC_CUDA_SUCCESS &&
			    api->cuDeviceGetCount(&count) == ZC_CUDA_SUCCESS) {
				for (int i = 0; i < count; i++) {
					CUdevice dev = 0;
					char luid[8] = {0};
					unsigned int node_mask = 0;
					if (api->cuDeviceGet(&dev, i) !=
						    ZC_CUDA_SUCCESS ||
					    api->cuDeviceGetLuid(luid, &node_mask,
								 dev) != ZC_CUDA_SUCCESS)
						continue;
					if (memcmp(luid, &ad.AdapterLuid, 8) == 0) {
						ordinal = i;
						break;
					}
				}
			}
			if (ordinal < 0)
				blog(LOG_WARNING,
				     "[obs-zcamera] cuda import: no CUDA device "
				     "matches the OBS D3D11 adapter LUID");
		}
		adapter->Release();
	}
	dxgi->Release();
	return ordinal;
}

/* Called by the decoder's destroy() (graphics thread). */
void zc_hw_cuda_import_free(struct zc_hw_cuda_import *imp)
{
	if (!imp)
		return;
	cuda_clear(imp);
	delete imp;
}

/* Called on the graphics thread, inside obs_enter_graphics(). `cuda_device` is
   the CUDA ordinal backing the D3D11 device (from zc_hw_cuda_device_for_d3d11),
   or -1 when unknown. Returns the OBS texture to draw, or NULL when this frame
   cannot be imported. */
gs_texture_t *zc_hw_cuda_import_frame(struct zc_hw_cuda_import **slot,
				      struct AVFrame *avf, int cuda_device,
				      struct ID3D11Device *device)
{
	if (!slot || !avf) {
		blog(LOG_WARNING, "[obs-zcamera] cuda import: null frame");
		return nullptr;
	}
	if (*slot && (*slot)->unavailable)
		return nullptr;
	if (cuda_device < 0 || !device) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: no CUDA device matching the OBS "
		     "adapter (device=%d)",
		     cuda_device);
		if (*slot)
			(*slot)->unavailable = true;
		return nullptr;
	}
	if (avf->format != AV_PIX_FMT_CUDA || !avf->data[0] || !avf->data[1]) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: frame is %s, expected CUDA planes",
		     av_get_pix_fmt_name((enum AVPixelFormat)avf->format));
		return nullptr;
	}

	const uint32_t width = (uint32_t)avf->width;
	const uint32_t height = (uint32_t)avf->height;
	if (!width || !height || (width & 1) || (height & 1)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: odd frame size %ux%u cannot be "
		     "converted",
		     width, height);
		return nullptr;
	}

	const zc_cuda_api *api = zc_cuda_api_get();
	if (!api) {
		if (*slot)
			(*slot)->unavailable = true;
		return nullptr;
	}

	if (!*slot) {
		*slot = new (std::nothrow) zc_hw_cuda_import;
		if (!*slot)
			return nullptr;
	}
	struct zc_hw_cuda_import *imp = *slot;

	if (imp->width != width || imp->height != height) {
		/* First frame, or the source renegotiated the resolution. */
		cuda_clear(imp);
		if (!cuda_setup(imp, api, device, cuda_device, width, height)) {
			imp->unavailable = true;
			cuda_clear(imp);
			return nullptr;
		}
	}

	if (!cuda_copy_planes(api, imp, avf))
		return nullptr;
	if (!cuda_render(avf, imp))
		return nullptr;

	gs_texture_t *tex =
		gs_texture_open_shared((uint32_t)(uintptr_t)imp->shared_handle);
	if (!tex) {
		blog(LOG_WARNING,
		     "[obs-zcamera] cuda import: gs_texture_open_shared failed "
		     "(handle %p)",
		     (void *)imp->shared_handle);
		return nullptr;
	}
	return tex;
}