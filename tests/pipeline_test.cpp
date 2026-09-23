/*
obs-zcamera — standalone end-to-end decode-pipeline test
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

--------------------------------------------------------------------------
Drives the plugin's REAL decode pipeline against a LOCAL video file, so the
pipeline can be verified before it is wired into OBS:

    file -> zc_hw_resolve() -> zc_hw_decoder_create()
         -> decode() -> import() -> gs texture -> release() -> destroy()

Unlike `decode_test.exe` (which re-implements the FFmpeg setup and therefore
can drift from the plugin), this links the actual plugin sources —
`src/hwdecode/hw_decode.c`, `hw_decode_ffmpeg.cpp` and `hw_decode_d3d11_win.cpp`.
Only the handful of OBS runtime symbols the decoder calls are shimmed below
(logging, the graphics-device accessor, the graphics-context enter/leave and the
shared-texture open/destroy). The shim's `gs_texture_open_shared` performs a
REAL `ID3D11Device::OpenSharedResource`, which is the same call libobs-d3d11
makes, so the shared-handle path is genuinely exercised.

The import assertion is the important one: the D3D11VA decoder emits NV12
*texture-array* slices that OBS cannot open, so `import()` must blit the slice
into a shareable non-array BGRA texture with an ID3D11VideoProcessor. The CUDA
decoder emits NV12 in CUDA device memory, which `import()` copies into
CUDA-registered D3D11 planes and converts with a pixel shader. This test
verifies the resulting texture is BGRA, single-slice, correctly sized, and that
its pixels are actually populated (a staging read-back), which is what proves
the conversion ran rather than silently producing a blank frame.

Both raw Annex-B streams and containers are accepted. The plugin's decode()
only takes raw access units, so container input (mp4/mkv/...) is passed through
the matching `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream filter first,
which turns the length-prefixed NALs back into Annex-B and re-inserts the
out-of-band SPS/PPS as in-band NALs. Raw streams are fed straight to decode()
without a filter.

Usage: pipeline_test [file.h264|file.mp4] [--max-frames N]
                     [--backend auto|d3d11va|cuda]
Exit code is non-zero when any check fails.
*/

#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>

#include "hwdecode/hw_decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

/* ================================================================== */
/* OBS runtime shim                                                    */
/*                                                                     */
/* The decoder sources are the real ones; only these symbols are       */
/* provided here instead of by libobs. `struct gs_texture` is          */
/* forward-declared by graphics.h, so the definition below is ours.    */
/* ================================================================== */
struct gs_texture {
	ID3D11Texture2D *tex;
};

static ID3D11Device *g_device = nullptr;

static int g_enter_graphics = 0;
static int g_leave_graphics = 0;
static int g_open_shared_calls = 0;

void blogva(int log_level, const char *format, va_list args)
{
	/* libobs log levels are 100/200/300/400 (util/base.h). */
	const char *lvl = "?";
	switch (log_level) {
	case LOG_ERROR:
		lvl = "ERROR";
		break;
	case LOG_WARNING:
		lvl = "WARNING";
		break;
	case LOG_INFO:
		lvl = "INFO";
		break;
	case LOG_DEBUG:
		lvl = "DEBUG";
		break;
	default:
		break;
	}
	std::printf("  [obs:%s] ", lvl);
	std::vprintf(format, args);
	std::printf("\n");
	std::fflush(stdout);
}

void blog(int log_level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	blogva(log_level, format, args);
	va_end(args);
}

void obs_enter_graphics(void)
{
	++g_enter_graphics;
}

void obs_leave_graphics(void)
{
	++g_leave_graphics;
}

/* In OBS this returns the ID3D11Device libobs-d3d11 renders with. Returning our
   standalone device is what makes the decoder allocate its surfaces on the same
   device the shared texture is opened on. */
void *gs_get_device_obj(void)
{
	return g_device;
}

gs_texture_t *gs_texture_open_shared(uint32_t handle)
{
	++g_open_shared_calls;
	if (!g_device || !handle)
		return nullptr;

	/* The same call libobs-d3d11 makes for a legacy shared handle. */
	ID3D11Texture2D *tex = nullptr;
	const HRESULT hr = g_device->OpenSharedResource(
		(HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D),
		(void **)&tex);
	if (FAILED(hr) || !tex)
		return nullptr;

	gs_texture *t = new (std::nothrow) gs_texture;
	if (!t) {
		tex->Release();
		return nullptr;
	}
	t->tex = tex;
	return t;
}

void gs_texture_destroy(gs_texture_t *tex)
{
	if (!tex)
		return;
	if (tex->tex)
		tex->tex->Release();
	delete tex;
}

/* ================================================================== */
/* tiny check harness                                                  */
/* ================================================================== */
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

static void check(bool ok, const std::string &what)
{
	if (ok) {
		++g_pass;
		std::printf("  [PASS] %s\n", what.c_str());
	} else {
		++g_fail;
		std::printf("  [FAIL] %s\n", what.c_str());
	}
	std::fflush(stdout);
}

static void skip(const std::string &what)
{
	++g_skip;
	std::printf("  [SKIP] %s\n", what.c_str());
	std::fflush(stdout);
}

/* Copy the imported texture back to the CPU and report whether it holds an
   actual image. A blank/unwritten texture would be uniformly 0. `out_color_delta`
   is the largest per-pixel distance from grey in the frame: it is non-zero only
   when the chroma plane made it through the conversion, so a luma-only result
   (all three channels equal) is caught. */
static bool read_back_is_populated(gs_texture_t *tex, uint32_t *out_min,
				   uint32_t *out_max, uint32_t *out_color_delta,
				   uint32_t *out_mean_r, uint32_t *out_mean_g,
				   uint32_t *out_mean_b)
{
	ID3D11DeviceContext *ctx = nullptr;
	g_device->GetImmediateContext(&ctx);
	if (!ctx)
		return false;

	D3D11_TEXTURE2D_DESC desc = {};
	tex->tex->GetDesc(&desc);

	D3D11_TEXTURE2D_DESC sd = desc;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.MiscFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ID3D11Texture2D *staging = nullptr;
	if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
		ctx->Release();
		return false;
	}

	ctx->CopyResource(staging, tex->tex);

	bool populated = false;
	uint32_t lo = 255, hi = 0, color_delta = 0;
	double sum_r = 0, sum_g = 0, sum_b = 0;
	uint64_t px_count = 0;
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		const uint8_t *base = (const uint8_t *)m.pData;
		for (uint32_t y = 0; y < desc.Height; ++y) {
			const uint8_t *row = base + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < desc.Width; ++x) {
				/* BGRA byte order. */
				const uint8_t b = row[x * 4 + 0];
				const uint8_t g = row[x * 4 + 1];
				const uint8_t r = row[x * 4 + 2];
				const uint8_t px[3] = {r, g, b};
				uint8_t mx = 0, mn = 255;
				for (int c = 0; c < 3; ++c) {
					if (px[c] > 0)
						populated = true;
					if (px[c] < lo)
						lo = px[c];
					if (px[c] > hi)
						hi = px[c];
					if (px[c] > mx)
						mx = px[c];
					if (px[c] < mn)
						mn = px[c];
				}
				if ((uint32_t)(mx - mn) > color_delta)
					color_delta = (uint32_t)(mx - mn);
				sum_r += r;
				sum_g += g;
				sum_b += b;
				++px_count;
			}
		}
		ctx->Unmap(staging, 0);
	}
	staging->Release();
	ctx->Release();
	if (out_min)
		*out_min = lo;
	if (out_max)
		*out_max = hi;
	if (out_color_delta)
		*out_color_delta = color_delta;
	if (px_count) {
		if (out_mean_r)
			*out_mean_r = (uint32_t)(sum_r / px_count);
		if (out_mean_g)
			*out_mean_g = (uint32_t)(sum_g / px_count);
		if (out_mean_b)
			*out_mean_b = (uint32_t)(sum_b / px_count);
	}
	return populated;
}

static enum zc_codec_id to_zc_codec(enum AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_HEVC:
		return ZC_CODEC_HEVC;
	case AV_CODEC_ID_H264:
	default:
		return ZC_CODEC_H264;
	}
}

/* True when avformat opened a raw Annex-B elementary stream (the "h264"/"hevc"
   demuxers). AVInputFormat::name is a comma-separated alias list, e.g.
   "h264,264". Such packets already carry in-band SPS/PPS and start codes, so
   they are exactly what decode() expects and need no bitstream filter. */
static bool is_raw_annexb_format(const AVFormatContext *fmt)
{
	if (!fmt->iformat || !fmt->iformat->name)
		return false;

	const char *p = fmt->iformat->name;
	while (*p) {
		const char *end = std::strchr(p, ',');
		const size_t len = end ? (size_t)(end - p) : std::strlen(p);
		if ((len == 4 && std::strncmp(p, "h264", 4) == 0) ||
		    (len == 4 && std::strncmp(p, "h265", 4) == 0) ||
		    (len == 4 && std::strncmp(p, "hevc", 4) == 0) ||
		    (len == 3 && std::strncmp(p, "264", 3) == 0) ||
		    (len == 3 && std::strncmp(p, "265", 3) == 0))
			return true;
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

/* The bitstream filter that converts a container's length-prefixed H.264/HEVC
   NALs back to Annex-B and restores the out-of-band SPS/PPS. Returns null for
   codecs with no such filter, which the caller reports as an error rather than
   silently feeding unfiltered packets to decode(). */
static const char *annexb_bsf_name(enum AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_H264:
		return "h264_mp4toannexb";
	case AV_CODEC_ID_HEVC:
		return "hevc_mp4toannexb";
	default:
		return nullptr;
	}
}

/* ================================================================== */
int main(int argc, char **argv)
{
	std::string path = "test_720p.h264";
	int max_frames = 0;
	enum zc_hw_backend requested = ZC_HW_AUTO;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--max-frames" && i + 1 < argc)
			max_frames = std::atoi(argv[++i]);
		else if (a == "--backend" && i + 1 < argc) {
			const std::string b = argv[++i];
			if (b == "auto")
				requested = ZC_HW_AUTO;
			else if (b == "d3d11va")
				requested = ZC_HW_D3D11VA;
			else if (b == "cuda")
				requested = ZC_HW_CUDA;
			else {
				std::fprintf(stderr, "unknown backend %s\n", b.c_str());
				return 2;
			}
		} else if (!a.empty() && a[0] != '-')
			path = a;
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}

	std::printf("=== pipeline_test: file=%s backend=%s ===\n", path.c_str(),
		    zc_hw_backend_name(requested));
	std::fflush(stdout);

	av_log_set_level(AV_LOG_ERROR);

	/* Stand in for the OBS graphics device. */
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				     D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
				     D3D11_SDK_VERSION, &g_device, nullptr,
				     nullptr)) ||
	    !g_device) {
		std::printf("  [SKIP] no D3D11 device (graphics-shim unavailable)\n");
		skip("decode pipeline (no D3D11 device)");
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return g_fail ? 1 : 0;
	}
	check(true, "standalone D3D11 device created (OBS device stand-in)");

	/* ---- open the local file ------------------------------------- */
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		std::printf("  [FAIL] cannot open %s\n", path.c_str());
		++g_fail;
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return 1;
	}
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		std::printf("  [FAIL] no stream info in %s\n", path.c_str());
		avformat_close_input(&fmt);
		++g_fail;
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return 1;
	}
	const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
					   nullptr, 0);
	if (vi < 0) {
		std::printf("  [FAIL] no video stream in %s\n", path.c_str());
		avformat_close_input(&fmt);
		++g_fail;
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return 1;
	}

	AVStream *vs = fmt->streams[vi];
	const int width = vs->codecpar->width;
	const int height = vs->codecpar->height;
	const enum zc_codec_id codec = to_zc_codec(vs->codecpar->codec_id);

	std::printf("  [file] %dx%d codec=%s\n", width, height,
		    codec == ZC_CODEC_HEVC ? "HEVC" : "H264");
	check(width > 0 && height > 0, "file reports coded dimensions");

	/* ---- packet path: raw Annex-B vs. container ------------------ */
	/* decode() only accepts raw access units, so container input has to go
	   through the matching *_mp4toannexb filter, which undoes the
	   length-prefixed framing and re-inserts the SPS/PPS the container kept
	   as extradata. Raw streams keep the direct path. */
	const bool raw_annexb = is_raw_annexb_format(fmt);
	const char *bsf_name =
		raw_annexb ? nullptr : annexb_bsf_name(vs->codecpar->codec_id);
	std::printf("  [input] format=%s -> %s\n",
		    fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "?",
		    raw_annexb ? "raw Annex-B (packets go straight to decode())"
			       : (bsf_name ? bsf_name
					   : "no Annex-B bitstream filter for this codec"));

	AVBSFContext *bsf = nullptr;
	if (!raw_annexb) {
		const AVBitStreamFilter *filter =
			bsf_name ? av_bsf_get_by_name(bsf_name) : nullptr;
		if (!filter) {
			std::printf("  [FAIL] no Annex-B bitstream filter for codec %s\n",
				    avcodec_get_name(vs->codecpar->codec_id));
			avformat_close_input(&fmt);
			++g_fail;
			std::printf("=== %d passed, %d failed, %d skipped ===\n",
				    g_pass, g_fail, g_skip);
			return 1;
		}
		int bret = av_bsf_alloc(filter, &bsf);
		if (bret >= 0)
			bret = avcodec_parameters_copy(bsf->par_in, vs->codecpar);
		if (bret >= 0) {
			bsf->time_base_in = vs->time_base;
			bret = av_bsf_init(bsf);
		}
		if (bret < 0) {
			std::printf("  [FAIL] cannot initialise the %s filter (%d)\n",
				    bsf_name, bret);
			av_bsf_free(&bsf);
			avformat_close_input(&fmt);
			++g_fail;
			std::printf("=== %d passed, %d failed, %d skipped ===\n",
				    g_pass, g_fail, g_skip);
			return 1;
		}
	}

	/* ---- resolve + create the real decoder ----------------------- */
	/* AUTO resolves to D3D11VA (the default the plugin ships with). */
	const enum zc_hw_backend expected =
		requested == ZC_HW_AUTO ? ZC_HW_D3D11VA : requested;
	const enum zc_hw_backend resolved = zc_hw_resolve(requested, codec);
	std::printf("  [resolve] %s -> %s\n", zc_hw_backend_name(requested),
		    zc_hw_backend_name(resolved));
	if (resolved != expected) {
		std::printf("  [SKIP] no %s hardware backend on this machine\n",
			    zc_hw_backend_name(expected));
		skip(std::string("hardware decode pipeline (no ") +
		     zc_hw_backend_name(expected) + " device)");
		avformat_close_input(&fmt);
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return g_fail ? 1 : 0;
	}
	check(true, std::string("zc_hw_resolve() selected the ") +
			    zc_hw_backend_name(resolved) + " backend");

	/* ten_bit=false: the harness exercises the 8-bit NV12 path (the fixture is
	   8-bit); a 10-bit/P010 frames context is not on test here. */
	struct zc_hw_decoder *dec =
		zc_hw_decoder_create(resolved, width, height, codec, false, nullptr);
	if (!dec) {
		check(false, "zc_hw_decoder_create() returned a decoder");
		avformat_close_input(&fmt);
		std::printf("=== %d passed, %d failed, %d skipped ===\n", g_pass,
			    g_fail, g_skip);
		return 1;
	}
	check(true, "zc_hw_decoder_create() returned a decoder");

	dec->set_video_params(dec, width, height, VIDEO_CS_DEFAULT,
			      VIDEO_RANGE_PARTIAL, (uint8_t)VIDEO_TRC_DEFAULT);

	/* ---- drive the file through decode()/import()/release() ------- */
	AVPacket *pkt = av_packet_alloc();
	long packets = 0, decoded = 0, hw_frames = 0, imported = 0;
	bool import_checked = false;
	uint32_t min_b = 255, max_b = 0, color_delta = 0;
	uint32_t mean_r = 0, mean_g = 0, mean_b = 0;

	/* Feed one Annex-B access unit to the real pipeline; returns true once
	   --max-frames has been satisfied. */
	auto feed = [&](const AVPacket *p) -> bool {
		++packets;
		struct zc_decoded_frame frame = {};
		bool got = false;
		const bool ok = dec->decode(
			dec, p->data, (size_t)p->size,
			(p->flags & AV_PKT_FLAG_KEY) != 0, p->pts, &frame, &got);
		if (!ok)
			std::printf("        decode() failed on packet %ld\n",
				    packets);
		if (got) {
			++decoded;
			if (frame.avframe)
				++hw_frames;

			/* Import on the "graphics thread"; OBS would then
			   draw the texture in video_render(). */
			if (!import_checked) {
				import_checked = true;
				const bool imported_ok =
					dec->import(dec, &frame);
				check(imported_ok,
				      "import() turned the decoded GPU frame into an OBS texture");
				if (imported_ok && frame.texture) {
					++imported;
					D3D11_TEXTURE2D_DESC d = {};
					frame.texture->tex->GetDesc(&d);
					std::printf(
						"  [texture] %ux%u fmt=%d "
						"array=%u miscFlags=0x%X\n",
						d.Width, d.Height,
						(int)d.Format,
						d.ArraySize,
						d.MiscFlags);
					check(d.Format ==
						      DXGI_FORMAT_B8G8R8A8_UNORM,
					      "imported texture is BGRA (a format OBS can sample)");
					check(d.ArraySize == 1,
					      "imported texture is a single 2D texture, not an array");
					check(d.Width == (UINT)width &&
						      d.Height == (UINT)height,
					      "imported texture matches the coded dimensions");
					check((d.MiscFlags &
					       D3D11_RESOURCE_MISC_SHARED) != 0,
					      "imported texture is shareable (came through gs_texture_open_shared)");
					const bool populated =
						read_back_is_populated(
							frame.texture,
							&min_b, &max_b,
							&color_delta,
							&mean_r, &mean_g,
							&mean_b);
					std::printf(
						"  [readback] populated=%s min=%u max=%u "
						"maxRGBspread=%u meanRGB=%u/%u/%u\n",
						populated ? "yes" : "no", min_b,
						max_b, color_delta, mean_r,
						mean_g, mean_b);
					check(populated,
					      "converted texture holds real pixels (not a blank frame)");
					/* A luma-only result (every pixel grey)
					   would mean the chroma plane was lost. */
					check(color_delta >= 8,
					      "converted texture carries chroma, not just luma");
					/* A single-channel cast (e.g. an all-green
					   frame from a chroma plane that read as 0)
					   still passes the spread check above, so
					   require every channel to carry content. */
					const uint32_t lo_mean = (mean_r < mean_g)
						 ? (mean_r < mean_b ? mean_r
								    : mean_b)
						 : (mean_g < mean_b ? mean_g
								    : mean_b);
					check(lo_mean >= 8,
					      "converted texture keeps the colour balance (no single-channel cast)");
				}
				/* The graphics context must be balanced. */
				check(g_enter_graphics == g_leave_graphics,
				      "obs_enter_graphics/obs_leave_graphics are balanced");
			}
			dec->release(dec, &frame);
		}
		return max_frames > 0 && decoded >= max_frames;
	};

	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vi) {
			if (!bsf) {
				const bool done = feed(pkt);
				av_packet_unref(pkt);
				if (done)
					break;
				continue;
			}

			/* Container input: av_bsf_send_packet() takes ownership of the
			   packet (and blanks it on success), so unref it either way.
			   One input packet can yield several outputs; the first one
			   already carries the SPS/PPS from codecpar. */
			if (av_bsf_send_packet(bsf, pkt) < 0) {
				std::printf("        av_bsf_send_packet() failed\n");
				av_packet_unref(pkt);
				continue;
			}
			av_packet_unref(pkt);

			bool done = false;
			AVPacket *conv = av_packet_alloc();
			if (conv) {
				while (av_bsf_receive_packet(bsf, conv) == 0) {
					done = feed(conv);
					av_packet_unref(conv);
					if (done)
						break;
				}
				av_packet_free(&conv);
			}
			if (done)
				break;
			continue;
		}
		av_packet_unref(pkt);
	}

	std::printf("  [result] packets=%ld decoded=%ld hw(%s)=%ld imported=%ld\n",
		    packets, decoded, zc_hw_backend_name(resolved), hw_frames,
		    imported);
	check(packets > 0, "the file produced video packets");
	check(decoded > 0, "decode() produced at least one frame");
	check(hw_frames > 0, std::string("frames came back as hardware ") +
				     zc_hw_backend_name(resolved) + " frames");
	check(g_open_shared_calls > 0,
	      "gs_texture_open_shared() was used for the zero-copy import");

	/* ---- teardown ------------------------------------------------- */
	dec->destroy(dec);
	av_packet_free(&pkt);
	av_bsf_free(&bsf);
	avformat_close_input(&fmt);
	check(true, "decoder destroyed without error");

	if (g_device)
		g_device->Release();
	avformat_network_deinit();

	std::printf("=== %s: %d passed, %d failed, %d skipped ===\n",
		    g_fail ? "FAILED" : "OK", g_pass, g_fail, g_skip);
	std::fflush(stdout);
	return g_fail ? 1 : 0;
}