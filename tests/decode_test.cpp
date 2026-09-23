/*
obs-zcamera — standalone D3D11VA decode probe
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
Reproduces the plugin's D3D11VA zero-copy decode setup in isolation (no
camera, no OBS): it decodes a LOCAL H.264/HEVC file and reports whether the
decoder produces hardware D3D11 frames (zero-copy capable) or falls back to
a software pix_fmt. Used to tell decoder-config bugs apart from camera stream
issues, and as a regression test for the hw-device-context wiring.

The probe runs a *matrix* of device/frames configurations so the exact cause
of a failure can be isolated. Modes:

  plugin     (default) mirrors src/hwdecode/hw_decode_ffmpeg.cpp exactly:
             a user D3D11 device is handed to FFmpeg through
             AVD3D11VADeviceContext.device only, plus a pre-created SHARED
             hw_frames_ctx (BIND_DECODER | MISC_SHARED).
  explicit   same, but AVD3D11VADeviceContext.device_context / video_device /
             video_context are populated by the caller before
             av_hwdevice_ctx_init().
  noframes   user device, but no pre-created hw_frames_ctx (FFmpeg owns it).
  ffmpeg     device created by av_hwdevice_ctx_create(), shared frames ctx.
  noshared   user device, shared frames ctx but WITHOUT MISC_SHARED.

Exit code is 0 when at least one frame decoded and no configuration the mode
requires failed; non-zero otherwise. A crash (access violation) means the
mode's device wiring is unusable — that is itself the test result.

Both raw Annex-B streams and containers are accepted: container input is passed
through the matching `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream filter
first, because an H.264/HEVC decoder only recognises start-code framed NALs
(and the container keeps SPS/PPS as out-of-band extradata). Raw streams are fed
straight to the decoder.

Usage: decode_test <file.h264|file.mp4> [mode] [--max-frames N] [--quiet]
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <d3d11.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace {

/* True when avformat opened a raw Annex-B elementary stream (the "h264"/"hevc"
   demuxers). AVInputFormat::name is a comma-separated alias list, e.g.
   "h264,264". Such packets already carry in-band SPS/PPS and start codes, so
   they need no bitstream filter. */
bool is_raw_annexb_format(const AVFormatContext *fmt)
{
	if (!fmt->iformat || !fmt->iformat->name)
		return false;

	const char *p = fmt->iformat->name;
	while (*p) {
		const char *end = strchr(p, ',');
		const size_t len = end ? (size_t)(end - p) : strlen(p);
		if ((len == 4 && strncmp(p, "h264", 4) == 0) ||
		    (len == 4 && strncmp(p, "h265", 4) == 0) ||
		    (len == 4 && strncmp(p, "hevc", 4) == 0) ||
		    (len == 3 && strncmp(p, "264", 3) == 0) ||
		    (len == 3 && strncmp(p, "265", 3) == 0))
			return true;
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

/* The bitstream filter that converts a container's length-prefixed H.264/HEVC
   NALs back to Annex-B and re-inserts the SPS/PPS kept as extradata. Returns
   null for codecs with no such filter. */
const char *annexb_bsf_name(enum AVCodecID id)
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

const char *fmt_name(AVPixelFormat f)
{
	const char *n = av_get_pix_fmt_name(f);
	return n ? n : "?";
}

struct Ctx {
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
	bool verbose = true;
};

/* Mirror the plugin's get_format callback (src/hwdecode/hw_decode_ffmpeg.cpp). */
enum AVPixelFormat get_format(AVCodecContext *avctx,
			      const enum AVPixelFormat *pix_fmts)
{
	Ctx *c = (Ctx *)avctx->opaque;
	enum AVPixelFormat hw = c ? c->hw_pix_fmt : AV_PIX_FMT_NONE;

	if (c && c->verbose) {
		std::string list;
		for (const enum AVPixelFormat *p = pix_fmts;
		     *p != AV_PIX_FMT_NONE; p++) {
			if (!list.empty())
				list += ' ';
			list += fmt_name(*p);
		}
		printf("[get_format] hw=%s offered=[%s]\n",
		       hw == AV_PIX_FMT_NONE ? "(none)" : fmt_name(hw),
		       list.c_str());
		fflush(stdout);
	}

	if (hw == AV_PIX_FMT_NONE)
		return pix_fmts[0];
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == hw)
			return hw;
	return pix_fmts[0];
}

int g_failures = 0;

void check(bool ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	fflush(stdout);
	if (!ok)
		g_failures++;
}

/* Report the hw device context state FFmpeg ended up with. */
void report_device_ctx(const char *tag, AVD3D11VADeviceContext *d3d)
{
	printf("  [ctx:%s] device=%s device_context=%s video_device=%s "
	       "video_context=%s\n",
	       tag, d3d && d3d->device ? "yes" : "NO",
	       d3d && d3d->device_context ? "yes" : "NO",
	       d3d && d3d->video_device ? "yes" : "NO",
	       d3d && d3d->video_context ? "yes" : "NO");
	fflush(stdout);
}

/* Inspect a decoded D3D11 frame: texture desc, array slice, shared handle. */
void inspect_hw_frame(AVFrame *frame, bool *shared_ok, bool *is_array,
		      bool *nv12)
{
	ID3D11Texture2D *tex = (ID3D11Texture2D *)frame->data[0];
	const int slice = (int)(intptr_t)frame->data[1];
	printf("  [hwframe] %dx%d slice=%d\n", frame->width, frame->height,
	       slice);
	fflush(stdout);
	if (!tex) {
		check(false, "D3D11 frame has data[0] texture");
		return;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	const bool arr = desc.ArraySize > 1;
	printf("  [hwframe] DXGI format=%d ArraySize=%u BindFlags=0x%X "
	       "MiscFlags=0x%X\n",
	       (int)desc.Format, desc.ArraySize, desc.BindFlags,
	       desc.MiscFlags);
	fflush(stdout);
	if (is_array)
		*is_array = arr;
	if (nv12)
		*nv12 = desc.Format == DXGI_FORMAT_NV12;

	IDXGIResource *res = nullptr;
	if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource),
					  (void **)&res)) &&
	    res) {
		HANDLE h = nullptr;
		const bool ok = SUCCEEDED(res->GetSharedHandle(&h)) && h != nullptr;
		res->Release();
		if (shared_ok)
			*shared_ok = ok;
		printf("  [hwframe] GetSharedHandle=%s\n", ok ? "ok" : "FAILED");
		fflush(stdout);
	} else {
		if (shared_ok)
			*shared_ok = false;
		printf("  [hwframe] IDXGIResource QI failed\n");
		fflush(stdout);
	}
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <file> [plugin|explicit|noframes|ffmpeg|noshared] "
			"[--max-frames N] [--quiet]\n",
			argv[0]);
		return 2;
	}

	const std::string path = argv[1];
	std::string mode = "plugin";
	int max_frames = 0;
	bool verbose = true;
	for (int i = 2; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--quiet")
			verbose = false;
		else if (a == "--max-frames" && i + 1 < argc)
			max_frames = atoi(argv[++i]);
		else if (!a.empty() && a[0] != '-')
			mode = a;
		else {
			fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}

	printf("=== decode_test mode=%s file=%s ===\n", mode.c_str(),
	       path.c_str());
	fflush(stdout);

	av_log_set_level(AV_LOG_ERROR);
	avformat_network_init();

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		fprintf(stderr, "cannot open %s\n", path.c_str());
		return 1;
	}
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		fprintf(stderr, "no stream info\n");
		avformat_close_input(&fmt);
		return 1;
	}
	const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
					   nullptr, 0);
	if (vi < 0) {
		fprintf(stderr, "no video stream\n");
		avformat_close_input(&fmt);
		return 1;
	}
	AVStream *vs = fmt->streams[vi];

	/* ---- packet path: raw Annex-B vs. container ------------------ */
	/* The decoder only accepts start-code framed NALs, so container input
	   must be converted first; the filter also re-inserts the SPS/PPS the
	   container kept as extradata. Raw streams keep the direct path. */
	const bool raw_annexb = is_raw_annexb_format(fmt);
	const char *bsf_name =
		raw_annexb ? nullptr : annexb_bsf_name(vs->codecpar->codec_id);
	printf("  [input] format=%s -> %s\n",
	       fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "?",
	       raw_annexb ? "raw Annex-B (packets go straight to the decoder)"
			  : (bsf_name ? bsf_name
				      : "no Annex-B bitstream filter for this codec"));
	fflush(stdout);

	AVBSFContext *bsf = nullptr;
	if (!raw_annexb) {
		const AVBitStreamFilter *filter =
			bsf_name ? av_bsf_get_by_name(bsf_name) : nullptr;
		if (!filter) {
			fprintf(stderr, "no Annex-B bitstream filter for codec %s\n",
				avcodec_get_name(vs->codecpar->codec_id));
			avformat_close_input(&fmt);
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
			fprintf(stderr, "cannot initialise the %s filter (%d)\n",
				bsf_name, bret);
			av_bsf_free(&bsf);
			avformat_close_input(&fmt);
			return 1;
		}
	}

	const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
	if (!codec) {
		fprintf(stderr, "no decoder for codec %d\n",
			vs->codecpar->codec_id);
		avformat_close_input(&fmt);
		return 1;
	}

	/* Verify the decoder exposes a D3D11VA hw config at all. */
	bool hw_config = false;
	for (int i = 0;; i++) {
		const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
		if (!cfg)
			break;
		if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
		    (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
			hw_config = true;
			break;
		}
	}
	check(hw_config, "decoder advertises D3D11VA hw device ctx");

	AVCodecContext *avctx = avcodec_alloc_context3(codec);
	if (!avctx) {
		avformat_close_input(&fmt);
		return 1;
	}
	avcodec_parameters_to_context(avctx, vs->codecpar);

	AVBufferRef *hw_dev = nullptr;
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *dctx = nullptr;

	const bool create_own_device = (mode != "ffmpeg");
	if (create_own_device) {
		/* The plugin hands FFmpeg the *OBS* device; standalone we create an
		   equivalent one (BGRA support matches what OBS requests). */
		D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
		const HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
			D3D11_SDK_VERSION, &dev, nullptr, &dctx);
		if (FAILED(hr) || !dev) {
			fprintf(stderr, "D3D11CreateDevice failed (0x%08lX)\n",
				(unsigned long)hr);
			avcodec_free_context(&avctx);
			avformat_close_input(&fmt);
			return 1;
		}
		printf("  [device] created standalone D3D11 device\n");

		AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
		if (!ref) {
			fprintf(stderr, "alloc d3d11va ctx failed\n");
			return 1;
		}
		AVHWDeviceContext *hwctx = (AVHWDeviceContext *)ref->data;
		AVD3D11VADeviceContext *d3d =
			(AVD3D11VADeviceContext *)hwctx->hwctx;

		/* What the plugin does today: only .device (with an extra ref, since
		   FFmpeg's device_free releases it). */
		d3d->device = dev;
		d3d->device->AddRef();

		if (mode == "explicit") {
			/* Populate the interfaces FFmpeg documents as "derived from
			   .device on init" so we can tell whether that derivation
			   actually happens in this FFmpeg build. */
			dev->GetImmediateContext(&d3d->device_context);
			dev->QueryInterface(__uuidof(ID3D11VideoDevice),
					    (void **)&d3d->video_device);
			if (d3d->device_context)
				d3d->device_context->QueryInterface(
					__uuidof(ID3D11VideoContext),
					(void **)&d3d->video_context);
		}

		if (av_hwdevice_ctx_init(ref) < 0) {
			fprintf(stderr, "av_hwdevice_ctx_init failed\n");
			return 1;
		}
		report_device_ctx("after-init", d3d);
		hw_dev = ref;
	} else {
		if (av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_D3D11VA,
					   nullptr, nullptr, 0) < 0) {
			fprintf(stderr, "av_hwdevice_ctx_create failed\n");
			return 1;
		}
		AVHWDeviceContext *hwctx = (AVHWDeviceContext *)hw_dev->data;
		report_device_ctx(
			"after-init",
			(AVD3D11VADeviceContext *)hwctx->hwctx);
	}

	avctx->hw_device_ctx = av_buffer_ref(hw_dev);
	avctx->opaque = new Ctx{AV_PIX_FMT_D3D11, verbose};
	avctx->get_format = get_format;

	/* Pre-create the frames context exactly like the plugin does. */
	const bool own_frames =
		(mode == "plugin" || mode == "explicit" || mode == "noshared");
	bool shared_frames_ok = false;
	if (own_frames) {
		avctx->hw_frames_ctx =
			av_hwframe_ctx_alloc(av_buffer_ref(hw_dev));
		if (avctx->hw_frames_ctx) {
			AVHWFramesContext *fc =
				(AVHWFramesContext *)avctx->hw_frames_ctx->data;
			AVD3D11VAFramesContext *dfc =
				(AVD3D11VAFramesContext *)fc->hwctx;
			fc->format = AV_PIX_FMT_D3D11;
			fc->sw_format = AV_PIX_FMT_NV12;
			fc->width = vs->codecpar->width;
			fc->height = vs->codecpar->height;
			dfc->BindFlags = D3D11_BIND_DECODER;
			dfc->MiscFlags = (mode == "noshared")
						 ? 0
						 : D3D11_RESOURCE_MISC_SHARED;
			const int fr =
				av_hwframe_ctx_init(avctx->hw_frames_ctx);
			shared_frames_ok = fr >= 0;
			printf("  [frames] init=%d %dx%d shared=%d\n", fr,
			       fc->width, fc->height,
			       dfc->MiscFlags & D3D11_RESOURCE_MISC_SHARED);
			fflush(stdout);
			if (fr < 0)
				av_buffer_unref(&avctx->hw_frames_ctx);
		}
	}
	if (mode != "ffmpeg")
		check(own_frames ? shared_frames_ok : true,
		      "hw_frames_ctx initialized");

	if (avcodec_open2(avctx, codec, nullptr) < 0) {
		fprintf(stderr, "avcodec_open2 failed\n");
		return 1;
	}
	printf("  [open] pix_fmt=%s hw_frames_ctx=%s\n",
	       fmt_name(avctx->pix_fmt),
	       avctx->hw_frames_ctx ? "yes" : "no");
	fflush(stdout);

	/* Decode. */
	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	int n_hw = 0, n_sw = 0, n_frames = 0;
	bool shared_ok = false, is_array = false, nv12 = false;
	bool inspected = false;

	/* Pull every frame the decoder has ready; returns true once
	   --max-frames has been satisfied. */
	auto drain = [&]() -> bool {
		while (avcodec_receive_frame(avctx, frame) == 0) {
			n_frames++;
			if (frame->format == AV_PIX_FMT_D3D11) {
				n_hw++;
				if (!inspected) {
					inspected = true;
					inspect_hw_frame(frame, &shared_ok,
							 &is_array, &nv12);
				}
			} else {
				n_sw++;
			}
			av_frame_unref(frame);
			if (max_frames > 0 && n_frames >= max_frames)
				return true;
		}
		return false;
	};

	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index != vi) {
			av_packet_unref(pkt);
			continue;
		}

		if (!bsf) {
			if (avcodec_send_packet(avctx, pkt) < 0) {
				av_packet_unref(pkt);
				continue;
			}
			av_packet_unref(pkt);
			if (drain())
				break;
			continue;
		}

		/* Container input: av_bsf_send_packet() takes ownership of the
		   packet (and blanks it on success), so unref it either way. One
		   input packet can yield several outputs; the first one already
		   carries the SPS/PPS from codecpar. */
		if (av_bsf_send_packet(bsf, pkt) < 0) {
			fprintf(stderr, "av_bsf_send_packet failed\n");
			av_packet_unref(pkt);
			continue;
		}
		av_packet_unref(pkt);

		bool done = false;
		AVPacket *conv = av_packet_alloc();
		if (conv) {
			while (av_bsf_receive_packet(bsf, conv) == 0) {
				if (avcodec_send_packet(avctx, conv) < 0) {
					av_packet_unref(conv);
					continue;
				}
				av_packet_unref(conv);
				if (drain()) {
					done = true;
					break;
				}
			}
			av_packet_free(&conv);
		}
		if (done)
			break;
	}
	/* Flush. */
	avcodec_send_packet(avctx, nullptr);
	while (avcodec_receive_frame(avctx, frame) == 0) {
		n_frames++;
		if (frame->format == AV_PIX_FMT_D3D11)
			n_hw++;
		else
			n_sw++;
		av_frame_unref(frame);
		if (max_frames > 0 && n_frames >= max_frames)
			break;
	}

	printf("  [result] frames=%d hw(D3D11)=%d software=%d\n", n_frames,
	       n_hw, n_sw);
	fflush(stdout);

	check(n_frames > 0, "at least one frame decoded");
	if (mode == "plugin" || mode == "explicit" || mode == "ffmpeg") {
		check(n_hw > 0, "hardware D3D11 frames produced");
	}
	if (n_hw > 0) {
		/* These two document *why* the Windows zero-copy import cannot use
		   gs_texture_open_shared() on the decoder texture (see README /
		   doc/design.md): the surface is a texture-array slice and its DXGI
		   format is NV12, which OBS's shared-texture path maps to GS_UNKNOWN. */
		printf("  [info] slice-aware import needed: %s; DXGI NV12: %s\n",
		       is_array ? "yes (ArraySize>1)" : "no",
		       nv12 ? "yes" : "no");
		printf("  [info] decoder texture shared handle: %s\n",
		       shared_ok ? "available (needs BGRA blt for OBS)"
				 : "unavailable");
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	av_bsf_free(&bsf);
	avcodec_free_context(&avctx);
	avformat_close_input(&fmt);
	av_buffer_unref(&hw_dev);
	avformat_network_deinit();
	if (dctx)
		dctx->Release();
	if (dev)
		dev->Release();

	printf("=== %s: %d failure(s) ===\n", g_failures ? "FAILED" : "OK",
	       g_failures);
	fflush(stdout);
	return g_failures ? 1 : 0;
}