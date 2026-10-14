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

/* See hw_decode_vt_mac.h for why macOS zero-copy goes through a native
   VideoToolbox session instead of FFmpeg's VideoToolbox hwaccel. */

#include "hw_decode_vt_mac.h"

#include <obs-module.h>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
/* CVPixelBufferGetIOSurface returns an IOSurfaceRef; CoreVideo usually pulls the
   header in, but do not depend on that. */
#import <IOSurface/IOSurfaceRef.h>

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

namespace zc {

namespace {

/* A NAL unit as AVCC expects it: the payload including its NAL header, with no
   Annex-B start code. */
struct NalRef {
	const uint8_t *data;
	size_t size;
};

/* Split an Annex-B elementary stream into NAL units. The SSP connector delivers
   Annex-B access units (that is also what the pipeline test feeds the decoder),
   so no AVCC input path is expected here. */
std::vector<NalRef> splitAnnexB(const uint8_t *p, size_t n)
{
	std::vector<NalRef> out;
	if (!p || n < 3)
		return out;

	/* Start code: 00 00 01 or 00 00 00 01. Returns its length at `at`. */
	auto codeLen = [&](size_t at) -> size_t {
		if (at + 3 <= n && p[at] == 0 && p[at + 1] == 0 && p[at + 2] == 1)
			return 3;
		if (at + 4 <= n && p[at] == 0 && p[at + 1] == 0 && p[at + 2] == 0 &&
		    p[at + 3] == 1)
			return 4;
		return 0;
	};

	size_t i = 0, code = 0;
	for (; i + 3 <= n; i++) {
		code = codeLen(i);
		if (code)
			break;
	}
	if (!code) {
		/* No start code anywhere. Guessing would turn an AVCC buffer (a
		   4-byte length where the NAL header belongs) into garbage, so
		   produce nothing and let the caller wait. */
		return out;
	}

	/* A NAL shorter than 2 bytes has no readable header; drop it instead of
	   letting it be read as type 0. */
	auto push = [&](size_t from, size_t to) {
		if (to > from && to - from >= 2)
			out.push_back({p + from, to - from});
	};

	size_t begin = i + code;
	i = begin;
	while (i + 3 <= n) {
		size_t l = codeLen(i);
		if (!l) {
			i++;
			continue;
		}
		push(begin, i);
		begin = i + l;
		i = begin;
	}
	push(begin, n);
	return out;
}

/* NAL type in the codec's own numbering. */
uint32_t nalType(enum zc_codec_id codec, const NalRef &nal)
{
	if (nal.size < 2)
		return 0;
	if (codec == ZC_CODEC_HEVC)
		return (uint32_t)((nal.data[0] >> 1) & 0x3F);
	return (uint32_t)(nal.data[0] & 0x1F);
}

/* Parameter-set NAL types, per codec. */
bool isParamSet(enum zc_codec_id codec, uint32_t type)
{
	if (codec == ZC_CODEC_HEVC)
		return type == 32 || type == 33 || type == 34; /* VPS SPS PPS */
	return type == 7 || type == 8; /* SPS PPS */
}

/* NAL types that are not part of the coded picture and must not be put in the
   sample buffer (VideoToolbox takes the parameter sets from the format
   description instead). */
bool skipInSample(enum zc_codec_id codec, uint32_t type)
{
	if (isParamSet(codec, type))
		return true;
	if (codec == ZC_CODEC_HEVC)
		return type >= 32; /* every non-VCL NAL: AUD/EOS/FD/SEI/filler */
	/* SEI, AUD, FDD, SEI-suffix, filler data: none of them picture data. */
	return type == 6 || type == 9 || type == 12 || type == 13 || type == 14;
}

} // namespace

struct zc_hw_vt_mac {
	struct zc_hw_decoder base;

	enum zc_codec_id codec = ZC_CODEC_H264;
	int width = 0, height = 0;
	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	enum video_range_type range = VIDEO_RANGE_PARTIAL;
	uint8_t trc = (uint8_t)VIDEO_TRC_DEFAULT;

	/* VideoToolbox state. The session is built from the parameter sets of the
	   first keyframe and rebuilt when they change (a resolution or profile
	   change mid-stream). */
	CMVideoFormatDescriptionRef fmt = nullptr;
	VTDecompressionSessionRef session = nullptr;
	std::vector<std::vector<uint8_t>> paramSets;

	/* The most recently decoded picture, retained here and read by import()
	   on the graphics thread. VideoToolbox hands frames to its output
	   callback; with a synchronous decode (flags 0) that happens before
	   VTDecompressionSessionDecodeFrame() returns. */
	std::mutex mtx;
	CVPixelBufferRef pending = nullptr;
	/* The buffer the OBS texture is currently bound to. Retained so the
	   pixel-buffer pool cannot recycle it while OBS is still sampling it. */
	CVPixelBufferRef bound = nullptr;

	/* One-shot log guards. Deliberately per-decoder members, like the FFmpeg
	   engine's: a function-local static would be shared by every source.
	   Atomic because the output callback runs on a VideoToolbox queue while the
	   others are written on the receive thread. */
	std::atomic<bool> logged_output_fail{false};
	std::atomic<bool> logged_no_iosurface{false};
	std::atomic<bool> logged_import_fail{false};
	std::atomic<bool> logged_size_mismatch{false};
	std::atomic<bool> logged_no_startcode{false};
	/* A session that could not be built is not retried per frame — the
	   attempt logs, and the stream delivers a frame every 33 ms. Cleared when
	   the parameter sets or the coded size change, which is the only thing that
	   could make the attempt succeed. */
	bool session_build_failed = false;

	/* The OBS texture the IOSurface is rebound into each frame — the same
	   pattern OBS's own macOS capture plugins use. Created and destroyed on
	   the graphics thread only. */
	gs_texture_t *tex = nullptr;
};

namespace {

void vt_output_callback(void *refCon, void *sourceFrameRefCon, OSStatus status,
			VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer,
			CMTime pts, CMTime duration)
{
	(void)sourceFrameRefCon;
	(void)infoFlags;
	(void)pts;
	(void)duration;

	zc_hw_vt_mac *v = (zc_hw_vt_mac *)refCon;
	if (!v)
		return;
	if (status != noErr || !imageBuffer) {
		if (!v->logged_output_fail) {
			v->logged_output_fail = true;
			blog(LOG_WARNING,
			     "[obs-zcamera] videotoolbox output failed: status %d",
			     (int)status);
		}
		return;
	}

	/* The callback's buffer is only valid during the call; keep our own
	   reference for the graphics thread. */
	CVPixelBufferRetain(imageBuffer);
	std::lock_guard<std::mutex> lock(v->mtx);
	if (v->pending)
		CVPixelBufferRelease(v->pending);
	v->pending = imageBuffer;
}

/* Build the format description from the collected parameter sets and open the
   decompression session. The destination is BGRA and IOSurface-backed, which is
   the only thing gs's OpenGL backend can bind (see the header). */
bool vt_build_session(zc_hw_vt_mac *v)
{
	if (v->paramSets.empty() || v->session_build_failed)
		return false;

	std::vector<const uint8_t *> addrs;
	std::vector<size_t> lens;
	addrs.reserve(v->paramSets.size());
	lens.reserve(v->paramSets.size());
	for (const std::vector<uint8_t> &ps : v->paramSets) {
		addrs.push_back(ps.data());
		lens.push_back(ps.size());
	}

	OSStatus st;
	if (v->codec == ZC_CODEC_HEVC)
		st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
			kCFAllocatorDefault, addrs.size(), addrs.data(),
			lens.data(), 4, false, &v->fmt);
	else
		st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
			kCFAllocatorDefault, addrs.size(), addrs.data(),
			lens.data(), 4, &v->fmt);
	if (st != noErr || !v->fmt) {
		v->session_build_failed = true;
		blog(LOG_WARNING,
		     "[obs-zcamera] videotoolbox: parameter sets rejected "
		     "(status %d, %zu sets)",
		     (int)st, addrs.size());
		return false;
	}

	/* Pure CoreFoundation, no Objective-C objects or bridge casts: this must
	   compile the same with and without -fobjc-arc, because the macos preset
	   uses the Xcode generator, which turns ARC on for the target
	   (cmake/macos/xcode.cmake). */
	SInt32 pixFmt = kCVPixelFormatType_32BGRA;
	CFNumberRef pixFmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixFmt);
	CFDictionaryRef emptyAttrs = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
							&kCFTypeDictionaryKeyCallBacks,
							&kCFTypeDictionaryValueCallBacks);
	CFDictionaryRef attrs = nullptr;
	if (pixFmtNum && emptyAttrs) {
		const void *attrKeys[] = {kCVPixelBufferPixelFormatTypeKey,
					  kCVPixelBufferIOSurfacePropertiesKey};
		const void *attrValues[] = {pixFmtNum, emptyAttrs};
		/* A CF dictionary aborts on a NULL value, so only build it when
		   both values exist. */
		attrs = CFDictionaryCreate(kCFAllocatorDefault, attrKeys, attrValues, 2,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	}
	if (!attrs) {
		v->session_build_failed = true;
		if (pixFmtNum)
			CFRelease(pixFmtNum);
		if (emptyAttrs)
			CFRelease(emptyAttrs);
		blog(LOG_WARNING,
		     "[obs-zcamera] videotoolbox: could not build the destination "
		     "pixel-buffer attributes");
		return false;
	}

	VTDecompressionOutputCallbackRecord cb = {vt_output_callback, v};
	st = VTDecompressionSessionCreate(kCFAllocatorDefault, v->fmt, nullptr, attrs,
					 &cb, &v->session);
	if (attrs)
		CFRelease(attrs);
	if (pixFmtNum)
		CFRelease(pixFmtNum);
	if (emptyAttrs)
		CFRelease(emptyAttrs);
	if (st != noErr || !v->session) {
		v->session_build_failed = true;
		blog(LOG_WARNING,
		     "[obs-zcamera] videotoolbox: BGRA decompression session "
		     "failed (status %d)",
		     (int)st);
		CFRelease(v->fmt);
		v->fmt = nullptr;
		return false;
	}

	blog(LOG_INFO,
	     "[obs-zcamera] videotoolbox zero-copy session ready: %dx%d %s -> "
	     "BGRA IOSurface",
	     v->width, v->height,
	     v->codec == ZC_CODEC_HEVC ? "HEVC" : "H.264");
	return true;
}

void vt_teardown_session(zc_hw_vt_mac *v)
{
	if (v->session) {
		VTDecompressionSessionInvalidate(v->session);
		CFRelease(v->session);
		v->session = nullptr;
	}
	if (v->fmt) {
		CFRelease(v->fmt);
		v->fmt = nullptr;
	}
	std::lock_guard<std::mutex> lock(v->mtx);
	if (v->pending) {
		CVPixelBufferRelease(v->pending);
		v->pending = nullptr;
	}
	if (v->bound) {
		CVPixelBufferRelease(v->bound);
		v->bound = nullptr;
	}
}

/* ---- backend interface ---- */

bool vt_decode(struct zc_hw_decoder *dec, const uint8_t *data, size_t size,
	       bool is_keyframe, int64_t pts, struct zc_decoded_frame *out,
	       bool *got_output)
{
	zc_hw_vt_mac *v = (zc_hw_vt_mac *)dec;
	*got_output = false;
	/* The parameter sets in the access unit decide when a session is built;
	   the caller's keyframe flag adds nothing here (and the macos-ci preset
	   builds with warnings as errors). */
	(void)is_keyframe;

	std::vector<NalRef> nals = splitAnnexB(data, size);
	if (nals.empty()) {
		if (!v->logged_no_startcode && size >= 3) {
			v->logged_no_startcode = true;
			blog(LOG_WARNING,
			     "[obs-zcamera] videotoolbox: no Annex-B start code in a "
			     "%zu-byte access unit; the SSP connector is expected to "
			     "deliver Annex-B, so nothing will decode",
			     size);
		}
		return true;
	}

	/* Collect parameter sets; a new SPS means the format description (and so
	   the session) has to be rebuilt. */
	bool sawSps = false;
	for (const NalRef &nal : nals) {
		uint32_t type = nalType(v->codec, nal);
		if (!isParamSet(v->codec, type))
			continue;
		if (v->codec == ZC_CODEC_HEVC ? type == 33 : type == 7)
			sawSps = true;
	}
	if (sawSps) {
		std::vector<std::vector<uint8_t>> fresh;
		for (const NalRef &nal : nals) {
			if (!isParamSet(v->codec, nalType(v->codec, nal)))
				continue;
			fresh.emplace_back(nal.data, nal.data + nal.size);
		}
		if (fresh != v->paramSets) {
			v->paramSets = std::move(fresh);
			v->session_build_failed = false;
			if (v->session)
				vt_teardown_session(v);
		}
	}

	if (!v->session) {
		if (v->paramSets.empty())
			return true; /* wait for the parameter sets of a keyframe */
		if (!vt_build_session(v))
			return false; /* failed once; latched until they change */
	}

	/* Rebuild the AVCC form of the access unit: every coded NAL, each prefixed
	   with a 4-byte length, exactly as the format description declared. */
	std::vector<uint8_t> avcc;
	size_t total = 0;
	for (const NalRef &nal : nals) {
		if (skipInSample(v->codec, nalType(v->codec, nal)))
			continue;
		total += 4 + nal.size;
	}
	if (!total)
		return true;
	avcc.reserve(total);
	for (const NalRef &nal : nals) {
		if (skipInSample(v->codec, nalType(v->codec, nal)))
			continue;
		uint32_t len = (uint32_t)nal.size;
		avcc.push_back((uint8_t)(len >> 24));
		avcc.push_back((uint8_t)(len >> 16));
		avcc.push_back((uint8_t)(len >> 8));
		avcc.push_back((uint8_t)len);
		avcc.insert(avcc.end(), nal.data, nal.data + nal.size);
	}

	CMBlockBufferRef bb = nullptr;
	OSStatus st = CMBlockBufferCreateWithMemoryBlock(
		kCFAllocatorDefault, nullptr, (CMSize)avcc.size(),
		kCFAllocatorDefault, nullptr, 0, (CMSize)avcc.size(),
		kCMBlockBufferAssureMemoryNowFlag, &bb);
	if (st != kCMBlockBufferNoErr || !bb) {
		blog(LOG_WARNING, "[obs-zcamera] videotoolbox: block buffer failed (%d)",
		     (int)st);
		return false;
	}
	if (CMBlockBufferReplaceDataBytes(bb, avcc.data(), 0, (CMSize)avcc.size()) !=
	    kCMBlockBufferNoErr) {
		CFRelease(bb);
		return false;
	}

	CMSampleTimingInfo timing;
	timing.duration = kCMTimeInvalid;
	timing.presentationTimeStamp = pts ? CMTimeMake(pts, 1000000) : kCMTimeInvalid;
	timing.decodeTimeStamp = kCMTimeInvalid;

	/* Nine arguments. The presentation time goes in the timing array, not in a
	   parameter of its own — the pts/duration form is CMSampleBufferCreate, the
	   one that also takes a make-data-ready callback. Zero sample-size entries
	   is correct: the sample size comes from the block buffer. */
	CMSampleBufferRef sb = nullptr;
	st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, v->fmt, 1, 1, &timing,
				       0, nullptr, &sb);
	CFRelease(bb);
	if (st != noErr || !sb) {
		blog(LOG_WARNING, "[obs-zcamera] videotoolbox: sample buffer failed (%d)",
		     (int)st);
		return false;
	}

	VTDecodeInfoFlags flagsOut = 0;
	/* Flags 0 means synchronous: the output callback runs on this thread
	   before this call returns. destroy() relies on that — it tears the
	   session down and frees the decoder immediately, which is only safe
	   while no callback can still be in flight. Moving to
	   kVTDecodeFrame_EnableAsynchronousDecompression would require the
	   callback to hold its own reference to the decoder. */
	st = VTDecompressionSessionDecodeFrame(v->session, sb, 0, nullptr, &flagsOut);
	CFRelease(sb);
	if (st != noErr) {
		if (!v->logged_output_fail) {
			v->logged_output_fail = true;
			blog(LOG_WARNING,
			     "[obs-zcamera] videotoolbox: DecodeFrame failed (%d); "
			     "further decode errors are not logged",
			     (int)st);
		}
		return false;
	}

	/* A synchronous decode (flags 0) delivers through the output callback
	   before this returns; only report a frame we actually have. */
	std::lock_guard<std::mutex> lock(v->mtx);
	if (!v->pending)
		return true;

	out->backend = ZC_HW_VIDEOTOOLBOX;
	/* The pixel buffer, not the meta: the meta may not have arrived yet, and
	   its 0 would make the source report a 0-size picture. */
	out->width = (uint32_t)CVPixelBufferGetWidth(v->pending);
	out->height = (uint32_t)CVPixelBufferGetHeight(v->pending);
	out->format = VIDEO_FORMAT_BGRA;
	out->cs = v->cs;
	out->range = v->range;
	out->trc = v->trc;
	/* The SSP pts is in microseconds and zc_decoded_frame timestamps are in
	   nanoseconds — the same conversion the FFmpeg engine applies. */
	out->timestamp_ns = pts > 0 ? (uint64_t)pts * 1000ULL : 0;
	out->avframe = nullptr; /* not an AVFrame: never av_frame_free() this */
	out->frame2 = nullptr;
	*got_output = true;
	return true;
}

bool vt_import(struct zc_hw_decoder *dec, struct zc_decoded_frame *frame)
{
	zc_hw_vt_mac *v = (zc_hw_vt_mac *)dec;

	std::lock_guard<std::mutex> lock(v->mtx);

	if (v->pending) {
		IOSurfaceRef surf = CVPixelBufferGetIOSurface(v->pending);
		if (!surf) {
			if (!v->logged_no_iosurface) {
				v->logged_no_iosurface = true;
				blog(LOG_WARNING,
				     "[obs-zcamera] videotoolbox: the decoded frame "
				     "is not IOSurface-backed, so it cannot be "
				     "imported without a copy; falling back");
			}
			return false;
		}

		if (!v->tex) {
			v->tex = gs_texture_create_from_iosurface(surf);
			if (!v->tex) {
				blog(LOG_WARNING,
				     "[obs-zcamera] videotoolbox: "
				     "gs_texture_create_from_iosurface failed — the "
				     "OpenGL backend rejects this IOSurface format");
				return false;
			}
		} else if (!gs_texture_rebind_iosurface(v->tex, surf)) {
			if (!v->logged_import_fail) {
				v->logged_import_fail = true;
				blog(LOG_WARNING,
				     "[obs-zcamera] videotoolbox: "
				     "gs_texture_rebind_iosurface failed; further "
				     "failures are not logged");
			}
			return false;
		}

		/* The texture now samples this IOSurface, so keep a reference to
		   it: the pool would otherwise recycle the buffer and overwrite
		   pixels OBS may still be reading. The retain moves from `pending`
		   to `bound`, so the count stays balanced. */
		if (v->bound)
			CVPixelBufferRelease(v->bound);
		v->bound = v->pending;
		v->pending = nullptr;
	} else if (!v->tex) {
		return false; /* nothing decoded yet */
	}
	/* No new picture this frame: the texture still holds the last one, and
	   `bound` keeps that buffer out of the pool, so drawing it again is safe.
	   Returning false here instead would trip the source's import-failure
	   fallback and drop the whole connection to software decode over a single
	   missing frame. */

	frame->texture = v->tex;
	/* The decoder owns the texture (it is rebound every frame and destroyed in
	   destroy()), so the source must not release it with the frame. */
	frame->owns_texture = false;
	frame->width = gs_texture_get_width(v->tex);
	frame->height = gs_texture_get_height(v->tex);

	/* VideoToolbox allocates its pixel buffers from the format description's
	   clean aperture, which should match the size the SSP meta reported. If it
	   does not, the picture is stretched by the coded/display difference, so
	   say so once instead of leaving it to be noticed as "the image looks
	   squashed". */
	if ((int)frame->width != v->width || (int)frame->height != v->height) {
		if (!v->logged_size_mismatch) {
			v->logged_size_mismatch = true;
			blog(LOG_WARNING,
			     "[obs-zcamera] videotoolbox: pixel buffer is %ux%u but "
			     "the stream meta says %dx%d — the picture will be "
			     "stretched",
			     frame->width, frame->height, v->width, v->height);
		}
	}
	return true;
}

void vt_set_video_params(struct zc_hw_decoder *dec, int width, int height,
			 enum video_colorspace cs, enum video_range_type range,
			 uint8_t trc)
{
	zc_hw_vt_mac *v = (zc_hw_vt_mac *)dec;
	bool resized = (width != v->width) || (height != v->height);
	v->width = width;
	v->height = height;
	v->cs = cs;
	v->range = range;
	v->trc = trc;
	/* The session allocates its pixel buffers from the format description, so
	   a coded-size change means rebuilding it on the next keyframe. */
	if (resized && v->session)
		vt_teardown_session(v);
}

void vt_release(struct zc_hw_decoder *dec, struct zc_decoded_frame *frame)
{
	/* Nothing to do: the pixel buffer stays owned by the decoder until the
	   next frame, and the texture is owned by the decoder too. */
	(void)dec;
	(void)frame;
}

void vt_destroy(struct zc_hw_decoder *dec)
{
	zc_hw_vt_mac *v = (zc_hw_vt_mac *)dec;
	vt_teardown_session(v);
	/* Runs on the graphics thread (the source destroys the decoder inside
	   obs_enter_graphics), like every other backend. */
	if (v->tex) {
		gs_texture_destroy(v->tex);
		v->tex = nullptr;
	}
	delete v;
}

} // namespace
} // namespace zc

extern "C" struct zc_hw_decoder *zc_hw_vt_mac_create(int width, int height,
						     enum zc_codec_id codec,
						     obs_source_t *source)
{
	(void)source;
	zc::zc_hw_vt_mac *v = new (std::nothrow) zc::zc_hw_vt_mac;
	if (!v)
		return nullptr;

	v->codec = codec;
	v->width = width;
	v->height = height;

	v->base.backend = ZC_HW_VIDEOTOOLBOX;
	v->base.decode = zc::vt_decode;
	v->base.import = zc::vt_import;
	v->base.set_video_params = zc::vt_set_video_params;
	v->base.release = zc::vt_release;
	v->base.destroy = zc::vt_destroy;
	return (struct zc_hw_decoder *)v;
}
