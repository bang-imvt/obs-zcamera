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

/* Clean rewrite of the ZCamera (SSP) OBS source.

   The inherited obs-ssp source was a ~39k-line monolith that coupled decode,
   sync, reconnect (guarded by a global static) and stream negotiation. This
   module is a focused obs_source_info implementation:

     camera -> ssp-connector subprocess -> SSPClientIso -> ZcHwDecoder
            (native zero-copy, or SW fallback)
            -> video_render (GPU texture)  |  obs_source_output_video2 (CPU)

   Per-instance reconnect, no global state. Kept from the reference: the
   ssp-connector subprocess isolation and mDNS discovery. */

#include <obs-module.h>
#include <obs.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <graphics/vec4.h>

/* swscale is only used by the CPU (Software) fallback path, which only exists
   when ENABLE_SW_DECODE is on — its FFmpeg::swscale link is added in the same
   CMake block. Keep the include behind the same guard so an
   ENABLE_SW_DECODE=OFF build still links. */
#ifdef ENABLE_SW_DECODE
extern "C" {
#include <libswscale/swscale.h>
}
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "ssp-client-iso.h"
#include "hwdecode/hw_decode.h"
#ifdef ENABLE_SW_DECODE
/* The audio path decodes AAC with the same helper the software video fallback
   uses; it is only compiled into a build that has FFmpeg (bugs 10/11). */
#include "ffmpeg-decode.h"
#endif
#include "obs-ssp.h"

/* libavcodec is needed for the AVFrame handed out by the zero-copy backends
   (ENABLE_ZERO_COPY links it) and for the CPU fallback (ENABLE_SW_DECODE). A
   build with both off carries no FFmpeg dependency at all. */
#if defined(ENABLE_ZERO_COPY) || defined(ENABLE_SW_DECODE)
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

namespace zc {

/* Release a decoded frame without going through the decoder. Used for frames
   that the source owns (pending render / teardown), so a concurrent decoder
   swap can never leave us calling into a destroyed decoder. Must run on the
   graphics thread when the frame owns a texture. */
static void release_frame_direct(struct zc_decoded_frame *frame)
{
#ifdef ENABLE_ZERO_COPY
	/* Only the hardware backends hand out an AVFrame. */
	if (frame->avframe) {
		AVFrame *avf = (AVFrame *)frame->avframe;
		av_frame_free(&avf);
		frame->avframe = nullptr;
	}
#endif
	if (frame->owns_texture && frame->texture) {
		gs_texture_destroy(frame->texture);
		frame->texture = nullptr;
		frame->owns_texture = false;
	}
}

} // namespace zc

#define PROP_SOURCE_IP "zc_source_ip"
#define PROP_DECODE_BACKEND "zc_decode_backend"
#define PROP_LOW_NOISE "zc_low_noise"
#define PROP_BITRATE "zc_bitrate"
#define PROP_WAIT_I_FRAME "zc_wait_i_frame"
#define PROP_SYNC_MODE "zc_sync_mode"
#define PROP_LED_TALLY "zc_led_tally"

#define SYNC_INTERNAL 0
#define SYNC_SSP_TIMESTAMP 1

/* How long a live connection may deliver no video at all before the source is
   rebuilt (see ZcSspSource::watchdog). The stream is silent between frames, so
   this has to be far longer than a frame interval: even a 1 fps camera is only
   silent for a second. */
#define ZC_STALL_TIMEOUT_MS 10000

namespace zc {

class ZcSspSource {
public:
	/* `async` selects the output mode. Render mode (default) is the existing
	   GPU source: video_render imports hardware surfaces. Async mode is the
	   10-bit/HDR source: it forces the CPU decoder and feeds native
	   P010/I010 obs_source_frame2 frames to obs_source_output_video2, so
	   OBS applies its P010 + HDR (PQ/HLG) GPU conversion. */
	explicit ZcSspSource(obs_source_t *source, bool async = false);
	~ZcSspSource();

	void update(obs_data_t *settings);
	void activate();
	void deactivate();
	void videoRender(gs_effect_t *effect);
	void videoTick(float seconds);
	void enumActiveSources(obs_source_enum_proc_t cb, void *param);
	uint32_t getWidth();
	uint32_t getHeight();

private:
	/* Pipeline lifecycle. */
	void start();
	void stop();
	void reconnectNow();
	void onDisconnected();
	void reconnectThread();
	/* Rebuilds the connection when the stream goes silent. */
	void watchdog();
	/* Stop + clear client_. Serialized by lifecycleMutex_ so the OBS
	   thread, the reconnect thread and the receive thread can never tear the
	   connection down concurrently (double pthread_join). */
	void teardownConnection();

	/* Decoder swap helpers. createDecoder() touches the OBS graphics device,
	   so it enters the graphics context itself; installDecoder() must NOT be
	   called with decoderMutex_ held (it destroys the old decoder, which
	   needs the graphics context). */
	struct zc_hw_decoder *createDecoder(int width, int height,
					    enum zc_codec_id codec, bool software,
					    bool tenBit);
	void installDecoder(struct zc_hw_decoder *fresh);

	/* Callback from the subprocess bridge. */
	void onVideoData(imf::SspH264Data *video);
	void onAudioData(imf::SspAudioData *audio);
	void onMeta(imf::SspVideoMeta *v, imf::SspAudioMeta *a, imf::SspMeta *m);

	/* Output a decoded frame. */
	void outputVideo(struct zc_decoded_frame *frame);

	obs_source_t *source_ = nullptr;
	std::string ip_;
	/* Output mode (see constructor): false = render-driven GPU source,
	   true = async obs_source_output_video2 source. Immutable per instance. */
	bool async_ = false;

	/* Decode backend. */
	enum zc_hw_backend requestedBackend_ = ZC_HW_AUTO; /* user setting */
	enum zc_hw_backend backend_ = ZC_HW_SOFTWARE;      /* resolved */
	struct zc_hw_decoder *decoder_ = nullptr;
	enum zc_codec_id codec_ = ZC_CODEC_H264;

	/* Stream bit depth, sniffed from the SPS because the SSP video meta has
	   no bit-depth field. Guarded by decoderMutex_. */
	bool bitDepthKnown_ = false;
	bool streamTenBit_ = false;
	/* The bit depth the current decoder_ was built with (drives rebuilds). */
	bool decoderTenBit_ = false;
	/* Last coded dimensions from onMeta, so a rebuild triggered on the
	   video-data path (before any frame is decoded) can recreate the
	   decoder at the right size. */
	int metaW_ = 0, metaH_ = 0;

	/* Stream settings. */
	int bitrate_ = 0;
	int syncMode_ = SYNC_INTERNAL;
	bool waitIFrame_ = false;
	bool lowNoise_ = false;

	/* Connection state. lifecycleMutex_ serializes start()/stop()/
	   teardownConnection() so the OBS thread, the reconnect thread and the
	   receive thread never stop client_ concurrently. */
	std::shared_ptr<SSPClientIso> client_;
	std::atomic<bool> running_{false};
	std::atomic<bool> iFrameShown_{false};
	std::mutex lifecycleMutex_;
	std::thread reconnectThread_;
	std::atomic<bool> reconnectJoined_{true};
	std::mutex cvMutex_;
	std::condition_variable cv_;
	bool stopping_ = false;

	/* Liveness timestamps for watchdog(). Written by the receive thread (and
	   start()), read by the graphics thread; both are plain nanosecond stamps,
	   so relaxed atomics are enough. startedNs_ is re-stamped on every
	   (re)connect, lastPacketNs_ is cleared with it so a connection that never
	   delivers a packet is measured from the connection itself. */
	std::atomic<uint64_t> startedNs_{0};
	std::atomic<uint64_t> lastPacketNs_{0};

	/* decoder_ lifetime. The receive thread (decode) and the graphics thread
	   (import) both hold this while touching decoder_, so a swap can never
	   free it under them. */
	std::mutex decoderMutex_;
	/* Set when a hardware surface cannot be imported: every later frame on
	   this connection is decoded by the software backend (design §9).
	   Guarded by decoderMutex_. */
	bool forceSoftware_ = false;

	/* GPU frame pending render, guarded by frameMutex_. The AVFrame is owned
	   by the source between decode (worker thread) and import (graphics
	   thread); the decoder's release() frees it once drawn. */
	std::mutex frameMutex_;
	struct zc_decoded_frame pending_ = {};
	bool hasPending_ = false;
	/* The imported picture currently on screen, guarded by frameMutex_. OBS
	   renders (and takes screenshots) faster than the camera produces frames,
	   so a render that finds no new frame must keep drawing this one instead
	   of clearing the canvas. Released on the graphics thread only, either in
	   videoRender() when it is replaced, or in stop()/~ZcSspSource(). */
	struct zc_decoded_frame drawn_ = {};
	enum video_format lastFormat_ = VIDEO_FORMAT_NONE;
	uint32_t lastWidth_ = 0, lastHeight_ = 0;

	/* CPU (Software) frame copy pending render, guarded by frameMutex_. The
	   NV12 planes from the decoder are converted to RGBA here so videoRender
	   can upload a single-plane texture reliably. */
	struct zc_cpu_frame {
		enum video_format format = VIDEO_FORMAT_NONE;
		uint32_t width = 0, height = 0;
		uint32_t linesize[MAX_AV_PLANES] = {};
		uint8_t *planes[MAX_AV_PLANES] = {};
		/* Bytes actually allocated per plane. linesize alone cannot detect
		   a height change, and copying the new (larger) size into an old
		   buffer would overflow it. */
		uint32_t planeSize[MAX_AV_PLANES] = {};
		enum video_range_type range = VIDEO_RANGE_PARTIAL;
#ifdef ENABLE_SW_DECODE
		/* RGBA conversion scratch + convertor. */
		uint8_t *rgba = nullptr;
		uint32_t rgbaSize = 0;
		struct SwsContext *sws = nullptr;
		enum video_format swsFmt = VIDEO_FORMAT_NONE;
		/* Dimensions the SwsContext was built for; a resolution change
		   must rebuild it. */
		uint32_t swsW = 0, swsH = 0;
#endif
	} cpuFrame_;
	bool hasCpuFrame_ = false;
	/* GPU texture for the converted CPU frame. Created and destroyed on the
	   graphics thread only (videoRender / ~ZcSspSource), so a concurrent
	   stop() can never leave a dangling texture being drawn. */
	gs_texture_t *cpuTex_ = nullptr;
	enum video_format lastCpuFmt_ = VIDEO_FORMAT_NONE;
	uint32_t cpuW_ = 0, cpuH_ = 0;

	/* Audio (bugs 10/11). The camera sends AAC access units over SSP; they are
	   decoded here and handed to OBS with obs_source_output_audio, which is
	   what gives the source a real audio track and a mixer control. Guarded by
	   audioMutex_: onAudioData runs on the SSP receive thread. Only a build
	   with FFmpeg has a decoder to run them through. */
#ifdef ENABLE_SW_DECODE
	std::mutex audioMutex_;
	struct ffmpeg_decode audioDec_ = {};
	bool audioDecReady_ = false;
	/* The encoder the camera reported in the audio meta. Only AAC is decoded;
	   anything else is reported once instead of being fed to the wrong
	   decoder. */
	uint32_t audioEncoder_ = 0;
	bool audioUnsupportedLogged_ = false;
#endif
};

/* ------------------------------------------------------------------ */
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

ZcSspSource::ZcSspSource(obs_source_t *source, bool async)
	: source_(source), async_(async)
{
}

ZcSspSource::~ZcSspSource()
{
	stop();
	/* The CPU texture is only ever touched on the graphics thread; the source
	   destructor runs after OBS has stopped rendering it, so this is the one
	   place besides videoRender() where cpuTex_ may be destroyed. */
	obs_enter_graphics();
	if (cpuTex_) {
		gs_texture_destroy(cpuTex_);
		cpuTex_ = nullptr;
	}
	obs_leave_graphics();
}

/* Sources added from the dock keep their address only in their own name
   ("ZCamera <ip>", spec §1 L3). A scene collection saved before
   zc_source_getdefaults() stopped clobbering create-time settings holds an
   empty zc_source_ip, so the source stays black on every later OBS launch
   until the operator re-adds it. Recover the address from the name instead,
   so such a source heals by itself. */
static std::string ipFromSourceName(obs_source_t *source)
{
	if (!source)
		return {};
	const char *name = obs_source_get_name(source);
	if (!name)
		return {};

	const std::string prefix = "ZCamera ";
	const std::string full = name;
	if (full.size() <= prefix.size() ||
	    full.compare(0, prefix.size(), prefix) != 0)
		return {};
	return full.substr(prefix.size());
}

void ZcSspSource::update(obs_data_t *settings)
{
	std::string ip = obs_data_get_string(settings, PROP_SOURCE_IP);
	/* A source saved with an empty address recovers it from its own name, so a
	   scene collection from before the get_defaults fix still renders. Applied
	   before the comparison below, so re-applying unchanged settings is still a
	   no-op and does not restart a healthy stream. */
	if (ip.empty())
		ip = ipFromSourceName(source_);
	enum zc_hw_backend backend = (enum zc_hw_backend)obs_data_get_int(
		settings, PROP_DECODE_BACKEND);
	/* CUDA/NVDEC is no longer offered in the UI (it renders green in OBS), but
	   a profile that saved backend=1 before the option was removed would still
	   ask for it here. Coerce it back to Auto so the broken path cannot be
	   re-enabled silently from an old profile. */
	if (backend == ZC_HW_CUDA)
		backend = ZC_HW_AUTO;
	int bitrate = (int)obs_data_get_int(settings, PROP_BITRATE);
	int syncMode = (int)obs_data_get_int(settings, PROP_SYNC_MODE);
	bool waitI = obs_data_get_bool(settings, PROP_WAIT_I_FRAME);
	bool lowNoise = obs_data_get_bool(settings, PROP_LOW_NOISE);

	bool changed = (ip != ip_) || (backend != requestedBackend_) ||
		       (bitrate != bitrate_) || (syncMode != syncMode_) ||
		       (waitI != waitIFrame_) || (lowNoise != lowNoise_);
	if (!changed)
		return;

	stop();
	ip_ = ip;
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		requestedBackend_ = backend;
	}
	bitrate_ = bitrate;
	syncMode_ = syncMode;
	waitIFrame_ = waitI;
	lowNoise_ = lowNoise;
	start();
}

void ZcSspSource::activate()
{
	if (!running_)
		start();
}

void ZcSspSource::deactivate()
{
	/* Keep the connection; OBS handles pausing. */
}

/* ------------------------------------------------------------------ */
/* Pipeline start/stop                                                 */
/* ------------------------------------------------------------------ */

void ZcSspSource::start()
{
	std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
	/* Backstop for a source whose name was not readable yet when update() ran. */
	if (ip_.empty())
		ip_ = ipFromSourceName(source_);
	if (ip_.empty() || running_)
		return;

	/* Decoder is created lazily in onMeta once the coded dimensions are known
	   (D3D11VA needs dims at open time to build a SHARED frames context).
	   Reconnect reaches start() again without going through stop(), so the
	   decoder from the previous connection must be destroyed here: merely
	   dropping the pointer would leak its hw device/frames context and GPU
	   textures on every reconnect. installDecoder(nullptr) does the swap under
	   decoderMutex_ and destroys the old decoder on the graphics thread. */
	installDecoder(nullptr);
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		forceSoftware_ = false;
		/* The bit depth is a fact about *this* connection, not the source: the
		   camera comes back with a different encoder when the operator changes
		   the codec, and the SPS sniff in onVideoData only runs while the depth
		   is still unknown. Keeping the previous connection's value here would
		   build the new decoder with the old depth, and the repair path could
		   not fire either — it compares against that same stale value. */
		bitDepthKnown_ = false;
		streamTenBit_ = false;
	}
	{
		/* stop() raises stopping_ to interrupt a reconnect thread; a fresh
		   start must clear it, or the next reconnect would abort at once. */
		std::lock_guard<std::mutex> lk(cvMutex_);
		stopping_ = false;
	}

	client_ = std::make_shared<SSPClientIso>(ip_, (uint32_t)(bitrate_ * 1024));

	/* Start the watchdog's stall clock for this connection attempt. */
	const uint64_t now = os_gettime_ns();
	startedNs_.store(now, std::memory_order_relaxed);
	lastPacketNs_.store(0, std::memory_order_relaxed);

	client_->setOnH264DataCallback(
	    [this](imf::SspH264Data *v) { onVideoData(v); });
	client_->setOnAudioDataCallback(
	    [this](imf::SspAudioData *a) { onAudioData(a); });
	client_->setOnMetaCallback(
	    [this](imf::SspVideoMeta *v, imf::SspAudioMeta *a, imf::SspMeta *m) {
		    onMeta(v, a, m);
	    });
	client_->setOnDisconnectedCallback([this]() { onDisconnected(); });
	/* The connector emits ConnectionConnectedMsg on the receive thread right
	   after TCP connect. It must be set, or OnConnectionConnected() would call
	   an empty std::function and throw bad_function_call on the receive thread
	   (uncaught -> terminate/crash). */
	client_->setOnConnectionConnectedCallback([]() {
		/* No-op: the connection being up is implicit in receiving callbacks. */
	});
	/* Defensive: every connector-emitted message must have a handler, or the
	   empty-std::function call from the receive thread throws bad_function_call
	   (uncaught -> terminate). */
	client_->setOnRecvBufferFullCallback([]() {
		blog(LOG_WARNING, "[obs-zcamera] connector receive buffer full");
	});
	client_->setOnExceptionCallback([](int code, const char *desc) {
		blog(LOG_WARNING, "[obs-zcamera] connector exception %d: %s", code,
		     desc ? desc : "");
	});

	running_ = true;
	iFrameShown_ = false;

	/* SSPClientIso wires Start->doStart internally in its constructor. */
	client_->Start();
}

void ZcSspSource::stop()
{
	/* Signal the reconnect thread to wake immediately (no 15 s block). */
	{
		std::lock_guard<std::mutex> lk(cvMutex_);
		stopping_ = true;
	}
	cv_.notify_all();

	teardownConnection();

	/* Join the reconnect thread so it never touches freed `this`. Done with
	   lifecycleMutex_ released: that thread may be inside reconnectNow(),
	   which takes it. */
	if (reconnectThread_.joinable() &&
	    reconnectThread_.get_id() != std::this_thread::get_id()) {
		reconnectThread_.join();
		reconnectJoined_ = true;
	}
	/* A reconnect that ran while we were stopping may have re-created the
	   connection; tear it down again so nothing is left running before the
	   decoder goes away. */
	teardownConnection();

	/* Drop an undrawn frame and the picture on screen. Released directly
	   rather than through decoder_, so a decoder swap racing this teardown
	   cannot leave a dangling call. */
	struct zc_decoded_frame dead = {};
	struct zc_decoded_frame last = {};
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (hasPending_) {
			dead = pending_;
			pending_ = {};
			hasPending_ = false;
		}
		last = drawn_;
		drawn_ = {};
	}
	if (dead.avframe || (dead.owns_texture && dead.texture)) {
		obs_enter_graphics();
		release_frame_direct(&dead);
		obs_leave_graphics();
	}
	if (last.avframe || (last.owns_texture && last.texture)) {
		obs_enter_graphics();
		release_frame_direct(&last);
		obs_leave_graphics();
	}

	struct zc_hw_decoder *dead_dec = nullptr;
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		dead_dec = decoder_;
		decoder_ = nullptr; /* nulled under the lock: no new user can get it */
		forceSoftware_ = false;
	}
	if (dead_dec) {
		/* Destroy GPU resources on the graphics thread. */
		obs_enter_graphics();
		zc_hw_decoder_destroy(dead_dec);
		obs_leave_graphics();
	}

	/* The AAC decoder holds no GPU resources, and the receive thread that
	   feeds it has already been joined. */
#ifdef ENABLE_SW_DECODE
	{
		std::lock_guard<std::mutex> lock(audioMutex_);
		if (audioDecReady_) {
			ffmpeg_decode_free(&audioDec_);
			audioDecReady_ = false;
		}
		audioEncoder_ = 0;
		audioUnsupportedLogged_ = false;
	}
#endif

	std::lock_guard<std::mutex> lock(frameMutex_);
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (cpuFrame_.planes[i]) {
			bfree(cpuFrame_.planes[i]);
			cpuFrame_.planes[i] = nullptr;
		}
		cpuFrame_.linesize[i] = 0;
		cpuFrame_.planeSize[i] = 0;
	}
#ifdef ENABLE_SW_DECODE
	if (cpuFrame_.rgba) {
		bfree(cpuFrame_.rgba);
		cpuFrame_.rgba = nullptr;
		cpuFrame_.rgbaSize = 0;
	}
	if (cpuFrame_.sws) {
		sws_freeContext(cpuFrame_.sws);
		cpuFrame_.sws = nullptr;
	}
	cpuFrame_.swsFmt = VIDEO_FORMAT_NONE;
	cpuFrame_.swsW = cpuFrame_.swsH = 0;
#endif
	hasCpuFrame_ = false;
	/* cpuTex_ is deliberately NOT destroyed here: it is only ever touched on
	   the graphics thread. videoRender() re-creates it when the frame format
	   or size changes, and ~ZcSspSource() releases the last one. */
}

/* Stop + clear client_. Serialized by lifecycleMutex_, so a second
   concurrent caller (stop() vs. reconnectNow()) finds it already null and
   never double-joins its thread. */
void ZcSspSource::teardownConnection()
{
	std::lock_guard<std::mutex> lock(lifecycleMutex_);
	running_ = false;
	if (client_)
		client_->Stop();
	client_.reset();
}

/* Reconnect without joining the reconnect thread (called from that thread). */
void ZcSspSource::reconnectNow()
{
	teardownConnection();
	if (stopping_) {
		/* stop() was requested while we were waking up; do not bring the
		   connection back up. */
		return;
	}
	/* Discard any frame decoded by the old decoder before start() resets it. */
	struct zc_decoded_frame dead = {};
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (hasPending_) {
			dead = pending_;
			pending_ = {};
			hasPending_ = false;
		}
	}
	if (dead.avframe || (dead.owns_texture && dead.texture)) {
		obs_enter_graphics();
		release_frame_direct(&dead);
		obs_leave_graphics();
	}
	start();
}

/* Build a decoder for the given codec. `software` forces the CPU backend (used
   by the import-failure fallback). Creating a hardware decoder reads the OBS
   graphics device, so this enters the graphics context itself; it must not be
   called with decoderMutex_ held, because installDecoder() (which does take it)
   needs the graphics context to destroy the old decoder and would otherwise
   deadlock against a rendering thread waiting for decoderMutex_. */
struct zc_hw_decoder *ZcSspSource::createDecoder(int width, int height,
						 enum zc_codec_id codec,
						 bool software, bool tenBit)
{
	enum zc_hw_backend requested;
	{
		/* Snapshot the user preference: update() may rewrite it from the
		   OBS thread while the graphics thread is here. */
		std::lock_guard<std::mutex> lock(decoderMutex_);
		requested = requestedBackend_;
	}
	/* The async source has no GPU surface to import (obs_source_output_video2
	   only accepts CPU frames), so it always decodes in software; the bit
	   depth then needs no special handling. */
	if (async_)
		software = true;
	enum zc_hw_backend backend =
		software ? zc_hw_resolve(ZC_HW_SOFTWARE, codec)
			 : zc_hw_resolve(requested, codec);
	struct zc_hw_decoder *dec = nullptr;
	obs_enter_graphics();
	dec = zc_hw_decoder_create(backend, width, height, codec, tenBit,
				   source_);
	obs_leave_graphics();
	{
		/* Record what was attempted, success or not, so diagnostics and
		   the fallback decision see the resolved backend. */
		std::lock_guard<std::mutex> lock(decoderMutex_);
		backend_ = backend;
	}
	return dec;
}

/* Swap in `fresh`, then destroy the previous decoder once no thread can reach
   it (both decode and import hold decoderMutex_ for their whole call, so
   acquiring it here waits them out). Must not be called with decoderMutex_
   held. */
void ZcSspSource::installDecoder(struct zc_hw_decoder *fresh)
{
	struct zc_hw_decoder *old = nullptr;
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		old = decoder_;
		decoder_ = fresh;
	}
	if (old) {
		obs_enter_graphics();
		zc_hw_decoder_destroy(old);
		obs_leave_graphics();
	}
}

/* Stream liveness watchdog, run from the render path.

   A camera or the connector can stop delivering video in the middle of a
   connection, and nothing in the SSP client reports that: no disconnect
   callback, no error, no timeout. The source then stays frozen on its last
   picture for good, which is exactly what made it unusable in practice.

   Every received packet stamps lastPacketNs_ (on the receive thread), and this
   check - which runs at the video frame rate whenever OBS renders the source -
   rebuilds the connection once that stamp is older than ZC_STALL_TIMEOUT_MS.
   It deliberately keys off received packets rather than decoded frames, so a
   camera that sends nothing at all (the connection comes up and stays silent)
   is covered by the same path via startedNs_. */
void ZcSspSource::watchdog()
{
	if (!running_)
		return;
	const uint64_t last = lastPacketNs_.load();
	const uint64_t since = last ? last : startedNs_.load();
	if (!since)
		return;
	const uint64_t silence_ms = (os_gettime_ns() - since) / 1000000ULL;
	if (silence_ms < ZC_STALL_TIMEOUT_MS)
		return;

	/* Re-arm before rebuilding: the reconnect takes seconds, and without this
	   every render until the first packet arrives would fire again. */
	lastPacketNs_.store(os_gettime_ns(), std::memory_order_relaxed);
	blog(LOG_WARNING,
	     "[obs-zcamera] no video data for %llu ms (%s); rebuilding the "
	     "connection",
	     (unsigned long long)silence_ms,
	     last ? "stream stalled" : "connection is silent");
	onDisconnected();
}

void ZcSspSource::onDisconnected()
{
	if (!running_)
		return;
	blog(LOG_INFO, "[obs-zcamera] disconnected, reconnecting...");
	/* Use a joinable member thread guarded so stop() can join it (no detached
	   thread touching freed `this`). Duplicate calls are ignored. */
	bool expected = true;
	if (!reconnectJoined_.compare_exchange_strong(expected, false))
		return;
	/* A previous reconnect thread is only joined by stop(), so by the time a
	   *second* disconnect arrives the finished thread is still joinable.
	   Assigning a new std::thread over a joinable one calls std::terminate(),
	   so retire it first. It has already published reconnectJoined_ = true
	   (that is what let this CAS succeed), i.e. it is at its last instruction,
	   so the join returns immediately. */
	if (reconnectThread_.joinable())
		reconnectThread_.join();
	reconnectThread_ = std::thread(&ZcSspSource::reconnectThread, this);
}

void ZcSspSource::reconnectThread()
{
	/* Escalating delay: 3 -> 6 -> 10 -> 15s. Interruptible so stop() returns
	   promptly instead of blocking the caller for up to 15 s. */
	static const int delays[] = {3000, 6000, 10000, 15000};
	for (int attempt = 0; running_; attempt++) {
		{
			std::unique_lock<std::mutex> lk(cvMutex_);
			if (cv_.wait_for(lk,
					 std::chrono::milliseconds(delays[attempt % 4]),
					 [this] { return stopping_; }))
				break; /* stop() requested */
		}
		if (!running_ || stopping_)
			break;
		reconnectNow();
		break;
	}
	{
		std::lock_guard<std::mutex> lk(cvMutex_);
		stopping_ = false;
	}
	reconnectJoined_ = true;
}

/* ------------------------------------------------------------------ */
/* Data callbacks                                                      */
/* ------------------------------------------------------------------ */

void ZcSspSource::onVideoData(imf::SspH264Data *video)
{
	if (!running_)
		return;
	/* Any packet proves the stream is alive, even one dropped below: the
	   watchdog in videoRender() only fires on real silence. */
	lastPacketNs_.store(os_gettime_ns(), std::memory_order_relaxed);
	if (waitIFrame_ && !iFrameShown_) {
		if (video->type == 5)
			iFrameShown_ = true;
		else
			return;
	}

	/* The SSP video meta has no bit-depth field, so sniff the SPS from the
	   elementary stream. A 10-bit stream that was started as 8-bit (the
	   decoder is built before the first SPS is seen) is rebuilt with a P010
	   frames context; software decode adapts to the stream itself and needs
	   no rebuild. */
	bool rebuildForBitDepth = false;
	bool tenBit = false;
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		if (decoder_ && !bitDepthKnown_) {
			int bd = zc_nal_bit_depth(video->data, video->len, codec_);
			if (bd >= 8) {
				bitDepthKnown_ = true;
				streamTenBit_ = (bd >= 10);
				blog(LOG_INFO,
				     "[obs-zcamera] stream bit depth: %d-bit (%s)",
				     bd, streamTenBit_ ? "10-bit" : "8-bit");
			}
		}
		if (bitDepthKnown_ && decoder_ && backend_ != ZC_HW_SOFTWARE &&
		    decoderTenBit_ != streamTenBit_) {
			rebuildForBitDepth = true;
			tenBit = streamTenBit_;
		}
	}
	if (rebuildForBitDepth) {
		/* Same teardown sequence as onMeta's rebuild: drop the pending
		   frame (released outside the locks, on the graphics thread) and
		   swap the decoder. */
		struct zc_decoded_frame dead = {};
		{
			std::lock_guard<std::mutex> lock(frameMutex_);
			if (hasPending_) {
				dead = pending_;
				pending_ = {};
				hasPending_ = false;
			}
		}
		if (dead.avframe || (dead.owns_texture && dead.texture)) {
			obs_enter_graphics();
			release_frame_direct(&dead);
			obs_leave_graphics();
		}
		blog(LOG_INFO,
		     "[obs-zcamera] rebuilding decoder for %s stream",
		     tenBit ? "10-bit" : "8-bit");
		struct zc_hw_decoder *fresh = nullptr;
		if (running_)
			fresh = createDecoder(metaW_, metaH_, codec_, false, tenBit);
		{
			std::lock_guard<std::mutex> lock(decoderMutex_);
			decoderTenBit_ = tenBit;
		}
		installDecoder(fresh);
	}

	struct zc_decoded_frame out = {};
	bool got = false;
	bool ok = false;
	{
		/* Enter the graphics context BEFORE decoderMutex_, so the lock order
		   is always graphics -> frameMutex_ -> decoderMutex_ (the order
		   videoRender() uses) and the two can never deadlock.
		   The hardware decoders drive the very same D3D11 immediate context
		   OBS renders with (the D3D11VA hwaccel is handed the OBS device),
		   and that context is not thread-safe: decoding and rendering it
		   concurrently hangs inside the driver and freezes the source for
		   good. Holding the graphics context across decode() serializes
		   them. */
#ifdef _WIN32
		obs_enter_graphics();
#endif
		{
			/* Hold decoderMutex_ across decode() so a concurrent swap
			   (stop(), an onMeta rebuild or the import-failure fallback)
			   cannot free the decoder under us. */
			std::lock_guard<std::mutex> lock(decoderMutex_);
			if (decoder_)
				ok = decoder_->decode(decoder_, video->data,
						      video->len, video->type == 5,
						      (int64_t)video->pts, &out,
						      &got);
			if (ok && got && async_) {
				/* Async 10-bit path: hand the CPU frame to OBS
				   while still holding decoderMutex_, so a decoder
				   swap cannot free the software decoder's frame
				   buffers under obs_source_output_video2 (which
				   copies the planes synchronously). OBS then runs
				   its own P010 + HDR (PQ/HLG) GPU conversion. */
				obs_source_frame2 *f2 = out.frame2;
				if (f2) {
					if (syncMode_ == SYNC_INTERNAL)
						f2->timestamp = os_gettime_ns();
					else
						f2->timestamp = out.timestamp_ns;
					obs_source_output_video2(source_, f2);
					lastWidth_ = f2->width;
					lastHeight_ = f2->height;
					got = false; /* already output below */
				}
			}
		}
#ifdef _WIN32
		obs_leave_graphics();
#endif
	}
	if (!ok || !got)
		return;

	if (syncMode_ == SYNC_INTERNAL)
		out.timestamp_ns = os_gettime_ns();

	outputVideo(&out);
}

void ZcSspSource::outputVideo(struct zc_decoded_frame *frame)
{
	/* Async mode already handed the frame to OBS inside onVideoData. */
	if (async_)
		return;
	if (frame->avframe) {
		/* Zero-copy GPU path: take ownership of the frame and hand it to
		   the graphics thread, which imports + draws + releases it. */
		struct zc_decoded_frame stale = {};
		{
			std::lock_guard<std::mutex> lock(frameMutex_);
			/* Drop a frame that was never drawn. Released outside the
			   lock and directly (not via decoder_), so we neither
			   destroy a texture while holding frameMutex_ from this
			   thread nor call into a decoder that may be swapped. */
			if (hasPending_) {
				stale = pending_;
				pending_ = {};
			}
			lastWidth_ = frame->width;
			lastHeight_ = frame->height;
			pending_ = *frame;
			hasPending_ = true;
		}
		if (stale.avframe || (stale.owns_texture && stale.texture)) {
			obs_enter_graphics();
			release_frame_direct(&stale);
			obs_leave_graphics();
		}
		return;
	}
	if (frame->frame2) {
#ifdef ENABLE_SW_DECODE
		/* CPU fallback: copy the planes into source-owned buffers, convert
		   to RGBA via swscale, then upload + draw in videoRender. */
		obs_source_frame2 *of = frame->frame2;
		std::lock_guard<std::mutex> lock(frameMutex_);
		zc_cpu_frame &c = cpuFrame_;
		c.format = of->format;
		c.width = of->width;
		c.height = of->height;
		c.range = of->range;
		lastWidth_ = of->width;
		lastHeight_ = of->height;

		/* Only planar 4:2:0 formats are fed to swscale; anything else is
		   dropped (no NULL-plane reads). 8-bit NV12/I420 plus 10-bit
		   P010/I010: 10-bit CPU frames come from software decoding a
		   10-bit stream (the GPU path labels them P010 too). */
		bool planar = (of->format == VIDEO_FORMAT_NV12 ||
			       of->format == VIDEO_FORMAT_I420 ||
			       of->format == VIDEO_FORMAT_P010 ||
			       of->format == VIDEO_FORMAT_I010);
		int nplanes;
		switch (of->format) {
		case VIDEO_FORMAT_NV12:
		case VIDEO_FORMAT_P010:
			nplanes = 2;
			break;
		case VIDEO_FORMAT_I420:
		case VIDEO_FORMAT_I010:
			nplanes = 3;
			break;
		default:
			nplanes = 1;
			break;
		}
		if (!planar || !of->data[0] || !of->linesize[0]) {
			hasCpuFrame_ = false;
			return;
		}
		for (int i = 0; i < nplanes; i++) {
			uint32_t ls = of->linesize[i];
			/* Plane 0 is full height; chroma planes are half for
			   4:2:0. */
			uint32_t ph = (i == 0) ? of->height : of->height / 2;
			uint32_t size = ls * ph;
			/* Reallocate whenever the buffer no longer matches, not
			   just when linesize changes: a height change with the
			   same stride must still grow the buffer or the memcpy
			   below overflows it. */
			if (!c.planes[i] || c.planeSize[i] < size) {
				c.planes[i] = c.planes[i]
						      ? (uint8_t *)brealloc(
								c.planes[i],
								size)
						      : (uint8_t *)bmalloc(
								size);
				c.planeSize[i] = size;
			}
			c.linesize[i] = ls;
			memcpy(c.planes[i], of->data[i], size);
		}
		/* Drop any stale extra planes from a prior format change. */
		for (int i = nplanes; i < MAX_AV_PLANES; i++) {
			if (c.planes[i]) {
				bfree(c.planes[i]);
				c.planes[i] = nullptr;
				c.planeSize[i] = 0;
				c.linesize[i] = 0;
			}
		}

		/* Convert to RGBA for a single-plane upload. */
		uint32_t rgbaSize = c.width * c.height * 4;
		if (!c.rgba || c.rgbaSize < rgbaSize) {
			if (c.rgba)
				bfree(c.rgba);
			c.rgba = (uint8_t *)bmalloc(rgbaSize);
			c.rgbaSize = rgbaSize;
		}
		/* Rebuild the convertor on a format OR resolution change: the
		   context is bound to the source dimensions it was created for. */
		if (!c.sws || c.swsFmt != c.format || c.swsW != c.width ||
		    c.swsH != c.height) {
			if (c.sws) {
				sws_freeContext(c.sws);
				c.sws = nullptr;
			}
			AVPixelFormat src;
			switch (c.format) {
			case VIDEO_FORMAT_NV12:
				src = AV_PIX_FMT_NV12;
				break;
			case VIDEO_FORMAT_P010:
				src = AV_PIX_FMT_P010LE;
				break;
			case VIDEO_FORMAT_I010:
				src = AV_PIX_FMT_YUV420P10LE;
				break;
			case VIDEO_FORMAT_I420:
			default:
				src = AV_PIX_FMT_YUV420P;
				break;
			}
			c.sws = sws_getContext((int)c.width, (int)c.height, src,
					       (int)c.width, (int)c.height,
					       AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
					       NULL, NULL, NULL);
			/* Tell swscale the source video range so 10-bit limited
			   frames do not come out lifted (and full-range frames
			   are not compressed). The RGBA output keeps the
			   default (limited, BT.601) mapping used for 8-bit
			   today. */
			if (c.sws) {
				const int *coeff =
					sws_getCoefficients(SWS_CS_DEFAULT);
				sws_setColorspaceDetails(
					c.sws, coeff,
					(c.range == VIDEO_RANGE_FULL), coeff, 0,
					0, 1 << 16, 1 << 16);
			}
			c.swsFmt = c.format;
			c.swsW = c.width;
			c.swsH = c.height;
		}
		if (c.sws) {
			uint8_t *src_data[4] = {c.planes[0], c.planes[1],
						c.planes[2], nullptr};
			int src_ls[4] = {(int)c.linesize[0], (int)c.linesize[1],
					 (int)c.linesize[2], 0};
			uint8_t *dst_data[4] = {c.rgba, nullptr, nullptr, nullptr};
			int dst_ls[4] = {(int)(c.width * 4), 0, 0, 0};
			sws_scale(c.sws, src_data, src_ls, 0, (int)c.height,
				  dst_data, dst_ls);
			hasCpuFrame_ = true;
		} else {
			hasCpuFrame_ = false;
		}
#else
		/* No software decoder is built; a CPU frame cannot occur. */
		(void)frame;
#endif
	}
}

void ZcSspSource::onAudioData(imf::SspAudioData *audio)
{
#ifdef ENABLE_SW_DECODE
	if (!running_ || !audio || !audio->data || audio->len == 0)
		return;

	/* The decoder is built on the first frame and lives until stop(): the
	   camera does not change its audio encoder mid-connection. */
	std::lock_guard<std::mutex> lock(audioMutex_);
	/* AAC is what the SSP stream carries. An encoder the camera did not report
	   is assumed to be AAC rather than dropping the audio; a known other one
	   (raw PCM) stays silent instead of being fed to the wrong decoder. */
	if (audioEncoder_ != AUDIO_ENCODER_UNKNOWN &&
	    audioEncoder_ != AUDIO_ENCODER_AAC) {
		if (!audioUnsupportedLogged_) {
			audioUnsupportedLogged_ = true;
			blog(LOG_WARNING,
			     "[obs-zcamera] SSP audio encoder %u is not supported; "
			     "the source will have no audio",
			     audioEncoder_);
		}
		return;
	}
	if (!audioDecReady_) {
		if (ffmpeg_decode_init(&audioDec_, AV_CODEC_ID_AAC, false) != 0) {
			blog(LOG_WARNING,
			     "[obs-zcamera] could not open the AAC decoder; the "
			     "source will have no audio");
			return;
		}
		audioDecReady_ = true;
	}

	struct obs_source_audio out = {};
	bool got = false;
	if (!ffmpeg_decode_audio(&audioDec_, audio->data, audio->len, &out,
				 &got))
		return;
	if (!got)
		return;

	/* Same clock convention as the video path: arrival time, unless the
	   operator asked for the SSP timestamps (whose pts is microseconds). */
	if (syncMode_ == SYNC_INTERNAL || audio->pts == 0)
		out.timestamp = os_gettime_ns();
	else
		out.timestamp = audio->pts * 1000ULL;

	/* `out.data` points into the decoder's frame, which the next decode
	   overwrites: obs_source_output_audio copies it before returning. */
	obs_source_output_audio(source_, &out);
#else
	/* This build has no FFmpeg, so the AAC stream cannot be decoded. */
	(void)audio;
#endif
}

void ZcSspSource::onMeta(imf::SspVideoMeta *v, imf::SspAudioMeta *a,
			 imf::SspMeta *m)
{
	(void)m;
#ifdef ENABLE_SW_DECODE
	if (a) {
		std::lock_guard<std::mutex> lock(audioMutex_);
		audioEncoder_ = a->encoder;
	}
#else
	(void)a;
#endif
	enum zc_codec_id codec =
		(v->encoder == VIDEO_ENCODER_H264) ? ZC_CODEC_H264 : ZC_CODEC_HEVC;

	/* Rebuild the decoder when the codec changes, it has not been created
	   yet, an import failure forced the software backend, or the stream now
	   has different coded dimensions. Creating here (not in start()) means
	   width/height are valid, so D3D11VA can build a SHARED frames context. */
	bool needRebuild;
	bool software;
	bool tenBit;
	{
		std::lock_guard<std::mutex> lock(decoderMutex_);
		software = forceSoftware_;
		/* A decoder bakes its coded dimensions into the frames context at
		   create time (D3D11VA needs them to build the SHARED pool), so a
		   stream that changes resolution has to be rebuilt around the new
		   size: set_video_params() only records the new size and would leave
		   the old surfaces in place. Comparing against the previous meta
		   covers both a change on the same connection and one that only
		   becomes visible after a reconnect. */
		const bool resized = decoder_ && (metaW_ != (int)v->width ||
						  metaH_ != (int)v->height);
		metaW_ = (int)v->width;
		metaH_ = (int)v->height;
		/* The bit depth may already be known (an earlier SPS on the same
		   connection); the decoder is also rebuilt on bit-depth flips in
		   onVideoData when it is only discovered there. */
		tenBit = bitDepthKnown_ ? streamTenBit_ : false;
		needRebuild = (decoder_ == nullptr) || (codec != codec_) || resized ||
			      (software && backend_ != ZC_HW_SOFTWARE);
		if (resized)
			blog(LOG_INFO,
			     "[obs-zcamera] stream resolution changed to %dx%d; "
			     "rebuilding the decoder",
			     (int)v->width, (int)v->height);
	}
	if (needRebuild) {
		/* Drop a frame decoded by the old decoder. */
		struct zc_decoded_frame dead = {};
		{
			std::lock_guard<std::mutex> lock(frameMutex_);
			if (hasPending_) {
				dead = pending_;
				pending_ = {};
				hasPending_ = false;
			}
		}
		if (dead.avframe || (dead.owns_texture && dead.texture)) {
			obs_enter_graphics();
			release_frame_direct(&dead);
			obs_leave_graphics();
		}

		struct zc_hw_decoder *fresh = nullptr;
		if (running_)
			fresh = createDecoder((int)v->width, (int)v->height, codec,
					      software, tenBit);
		{
			std::lock_guard<std::mutex> lock(decoderMutex_);
			codec_ = codec;
			decoderTenBit_ = tenBit;
			if (!fresh) {
				blog(LOG_WARNING,
				     "[obs-zcamera] no decoder available "
				     "(%dx%d %s)",
				     (int)v->width, (int)v->height,
				     zc_hw_backend_name(backend_));
			}
		}
		installDecoder(fresh);
	}

	std::lock_guard<std::mutex> lock(decoderMutex_);
	if (decoder_)
		decoder_->set_video_params(decoder_, (int)v->width,
					   (int)v->height, VIDEO_CS_DEFAULT,
					   VIDEO_RANGE_PARTIAL,
					   (uint8_t)VIDEO_TRC_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

void ZcSspSource::videoRender(gs_effect_t *effect)
{
	gs_texture_t *tex = NULL;
	bool fallbackToSoftware = false;
	int frameW = 0, frameH = 0;
	/* Runs before the early returns below, so a stream that never produces a
	   frame is noticed too. */
	watchdog();
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (hasPending_ && pending_.avframe) {
			/* Zero-copy GPU path: import the surface into an OBS
			   texture. decoderMutex_ is held only for the import, so
			   a decoder swap waits for it and vice versa. */
			bool imported = false;
			{
				std::lock_guard<std::mutex> dlock(decoderMutex_);
				imported = decoder_ &&
					   decoder_->import(decoder_, &pending_);
			}
			if (!imported) {
				/* The surface cannot be imported into OBS (no
				   shared handle / unsupported slice). Drop the
				   frame and fall back to software decode for the
				   rest of this connection (design §9). */
				release_frame_direct(&pending_);
				pending_ = {};
				hasPending_ = false;
				{
					std::lock_guard<std::mutex> dlock(
						decoderMutex_);
					fallbackToSoftware = !forceSoftware_;
					forceSoftware_ = true;
				}
			} else {
				/* Take ownership and drop the frame from the
				   shared slot BEFORE drawing, so the decode
				   thread cannot release/destroy the texture we
				   are about to draw. The frame stays as the
				   picture to draw until a newer one replaces it;
				   the one it replaces is released here, on the
				   graphics thread. */
				struct zc_decoded_frame previous = drawn_;
				drawn_ = pending_;
				pending_ = {};
				hasPending_ = false;
				if (previous.avframe ||
				    (previous.owns_texture && previous.texture))
					release_frame_direct(&previous);
			}
			frameW = (int)lastWidth_;
			frameH = (int)lastHeight_;
		}
#ifdef ENABLE_SW_DECODE
		else if (hasCpuFrame_) {
			/* CPU fallback: upload the converted RGBA frame. */
			zc_cpu_frame &c = cpuFrame_;
			if (!c.width || !c.height || !c.rgba) {
				hasCpuFrame_ = false;
				return;
			}
			if (!cpuTex_ || c.format != lastCpuFmt_ ||
			    c.width != cpuW_ || c.height != cpuH_) {
				if (cpuTex_) {
					gs_texture_destroy(cpuTex_);
					cpuTex_ = NULL;
				}
				cpuTex_ = gs_texture_create(c.width, c.height,
							    GS_RGBA, 1, NULL,
							    GS_DYNAMIC);
				lastCpuFmt_ = c.format;
				cpuW_ = c.width;
				cpuH_ = c.height;
			}
			if (!cpuTex_) {
				hasCpuFrame_ = false;
				return;
			}
			gs_texture_set_image(cpuTex_, c.rgba, c.width * 4, false);
			tex = cpuTex_;
			hasCpuFrame_ = false;
			/* The GPU picture is superseded for good (the software
			   fallback is permanent): release it rather than leave a
			   stale frame that a later render would fall back to. */
			if (drawn_.avframe ||
			    (drawn_.owns_texture && drawn_.texture)) {
				release_frame_direct(&drawn_);
				drawn_ = {};
			}
		}
#endif
		/* No new frame arrived for this render: keep showing the last
		   decoded picture. */
		if (!tex)
			tex = drawn_.texture;
	} /* end frameMutex_ lock scope */

	if (fallbackToSoftware) {
		blog(LOG_WARNING,
		     "[obs-zcamera] GPU surface import failed; switching this "
		     "connection to software decode");
		enum zc_codec_id codec;
		{
			std::lock_guard<std::mutex> lock(decoderMutex_);
			codec = codec_;
		}
		struct zc_hw_decoder *fresh = nullptr;
		if (running_) {
			bool tenBit;
			{
				std::lock_guard<std::mutex> lock(decoderMutex_);
				tenBit = bitDepthKnown_ ? streamTenBit_ : false;
			}
			fresh = createDecoder(frameW, frameH, codec, true, tenBit);
		}
		installDecoder(fresh);
		if (!fresh) {
			blog(LOG_WARNING,
			     "[obs-zcamera] no software decoder available; video "
			     "output stays black until the decoder is rebuilt");
		}
		/* Nothing to draw this frame; the next decoded frame is a CPU
		   frame handled by the branch above. */
		return;
	}

	if (!tex)
		return;

	/* Draw the texture. */
	gs_effect_t *draw_effect = (effect) ? effect : gs_get_effect();
	if (draw_effect) {
		gs_effect_set_texture(
			gs_effect_get_param_by_name(draw_effect, "image"), tex);
		gs_draw_sprite(tex, 0, 0, 0);
	}
}

void ZcSspSource::videoTick(float seconds)
{
	(void)seconds;
	watchdog();
}

void ZcSspSource::enumActiveSources(obs_source_enum_proc_t cb, void *param)
{
	(void)cb;
	(void)param;
}

uint32_t ZcSspSource::getWidth()
{
	std::lock_guard<std::mutex> lock(frameMutex_);
	return lastWidth_;
}

uint32_t ZcSspSource::getHeight()
{
	std::lock_guard<std::mutex> lock(frameMutex_);
	return lastHeight_;
}

/* ------------------------------------------------------------------ */
/* obs_source_info glue                                                */
/* ------------------------------------------------------------------ */

static const char *zc_source_getname(void *unused)
{
	(void)unused;
	/* Plain "ZCamera": this is what shows in OBS's "Add Source" menu and as the
	   default name of a manually-added source. The dock names its sources
	   "ZCamera <ip>"; the base name must not be a raw locale key. */
	return "ZCamera";
}

static obs_properties_t *zc_source_getproperties(void *unused)
{
	(void)unused;
	/* A ZCamera source is really driven from the control dock (discovery,
	   add-to-scene, settings, PTZ); the classic properties dialog is kept
	   minimal so it cannot shadow that, and opening it (which happens both when
	   the source is added and when Properties is clicked) raises the dock so the
	   operator lands on the surface that controls the camera. */
	zc_show_control_dock();

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, PROP_SOURCE_IP,
				obs_module_text("SSPPlugin.SourceProps.SourceIp"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(
	    props, "zc_dock_hint",
	    obs_module_text("ZCameraPlugin.SourceProps.DockHint"),
	    OBS_TEXT_INFO);
	return props;
}

static void zc_source_getdefaults(obs_data_t *settings)
{
	/* Must be obs_data_set_default_*, not obs_data_set_*: OBS merges the
	   caller's settings into the source's data object and only then calls
	   get_defaults (obs_source_create_internal -> obs_source_init_context ->
	   get_defaults). Setting the keys outright would overwrite whatever
	   obs_source_create() was given, so a source created with an IP -- which is
	   exactly what the dock's "Add to Source" does -- would come back with an
	   empty one and never connect. */
	obs_data_set_default_string(settings, PROP_SOURCE_IP, "");
	obs_data_set_default_int(settings, PROP_DECODE_BACKEND, ZC_HW_AUTO);
	obs_data_set_default_int(settings, PROP_BITRATE, 20);
	obs_data_set_default_int(settings, PROP_SYNC_MODE, SYNC_INTERNAL);
	obs_data_set_default_bool(settings, PROP_WAIT_I_FRAME, false);
	obs_data_set_default_bool(settings, PROP_LED_TALLY, false);
	obs_data_set_default_bool(settings, PROP_LOW_NOISE, false);
}

static void *zc_source_create(obs_data_t *settings, obs_source_t *source)
{
#ifdef __APPLE__
	ZcSspSource *s = new (std::nothrow) ZcSspSource(source, true);
#else
	ZcSspSource *s = new (std::nothrow) ZcSspSource(source);
#endif
	if (!s)
		return nullptr;
	s->update(settings);
	return s;
}

static void zc_source_destroy(void *data)
{
	delete static_cast<ZcSspSource *>(data);
}

static void zc_source_update(void *data, obs_data_t *settings)
{
	static_cast<ZcSspSource *>(data)->update(settings);
}

static void zc_source_activate(void *data)
{
	static_cast<ZcSspSource *>(data)->activate();
}

static void zc_source_deactivate(void *data)
{
	static_cast<ZcSspSource *>(data)->deactivate();
}

static void zc_source_video_render(void *data, gs_effect_t *effect)
{
	static_cast<ZcSspSource *>(data)->videoRender(effect);
}

static void zc_source_video_tick(void *data, float seconds)
{
	static_cast<ZcSspSource *>(data)->videoTick(seconds);
}

static uint32_t zc_source_get_width(void *data)
{
	return static_cast<ZcSspSource *>(data)->getWidth();
}

static uint32_t zc_source_get_height(void *data)
{
	return static_cast<ZcSspSource *>(data)->getHeight();
}

static void zc_source_enum_active_sources(void *data,
					  obs_source_enum_proc_t cb,
					  void *param)
{
	static_cast<ZcSspSource *>(data)->enumActiveSources(cb, param);
}

} // namespace zc

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

struct obs_source_info create_ssp_source_info()
{
	struct obs_source_info info = {};
	info.id = "zcamera_source"; /* unique; avoids clash with legacy obs-ssp */
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* The camera's audio arrives on the same SSP connection and is decoded into
	   obs_source_output_audio, so the source carries a real audio track (and a
	   mixer control) instead of a permanently silent one (bugs 10/11). */
#ifdef __APPLE__
	/* The OpenGL backend cannot import the camera's 420v IOSurface. Decode in
	   software and let OBS upload/convert the frame asynchronously. */
	info.output_flags =
		OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
#else
	info.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
#endif
	info.get_name = zc::zc_source_getname;
	info.get_properties = zc::zc_source_getproperties;
	info.get_defaults = zc::zc_source_getdefaults;
	info.create = zc::zc_source_create;
	info.destroy = zc::zc_source_destroy;
	info.update = zc::zc_source_update;
	info.activate = zc::zc_source_activate;
	info.deactivate = zc::zc_source_deactivate;
#ifdef __APPLE__
	info.video_tick = zc::zc_source_video_tick;
#else
	info.video_render = zc::zc_source_video_render;
#endif
	info.get_width = zc::zc_source_get_width;
	info.get_height = zc::zc_source_get_height;
	info.enum_active_sources = zc::zc_source_enum_active_sources;
	return info;
}
