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
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "ssp-client-iso.h"
#include "hwdecode/hw_decode.h"
/* Pointer interaction with the Interact window: the source forwards clicks
   and the dock answers with the camera writes (see the header). */
#include "zc_source_bridge.h"
#ifdef ENABLE_SW_DECODE
/* The audio path decodes AAC with the same helper the software video fallback
   uses; it is only compiled into a build that has FFmpeg (bugs 10/11). */
#include "ffmpeg-decode.h"
#endif
#include "obs-ssp.h"

#ifdef ENABLE_CAMERA_CONTROL
/* The cameractrl sources are compiled into this plugin target only when camera
   control is on (CMakeLists), which is what these #ifdef guards mirror. The
   source itself writes nothing to the camera — the dock owns that — but the AI
   overlay reads the camera's notification socket directly (see
   ZcSspSource::startAiFeed), so it needs the notifier and its payload parser. */
#include "cameractrl/zc_settings_schema.h"
#include "cameractrl/zc_ws_notifier.h"
#include "cameractrl/zc_ai_detect.h"
#include <QThread>
#include <QSet>
#include <QRegularExpression>
#endif

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
#define PROP_WAIT_I_FRAME "zc_wait_i_frame"
#define PROP_SYNC_MODE "zc_sync_mode"
#define PROP_LED_TALLY "zc_led_tally"
/* Draw the camera's AI detection boxes (people / heads / faces) over this
   source's picture. Only offered when camera control is compiled in. This is
   the master switch: with it unticked drawAiOverlay() draws nothing at all,
   whatever the camera is reporting — see ZcSspSource::drawAiOverlay. */
#define PROP_AI_OVERLAY "zc_ai_overlay"
/* Where the boxes may be drawn: a box in the program output is also a box in
   the recording and the stream, and only a box in the program output is
   visible in the standard preview pane. */
#define PROP_AI_OVERLAY_MODE "zc_ai_overlay_mode"

/* Values of PROP_AI_OVERLAY_MODE. PROGRAM draws the boxes into the picture
   itself, so they show up wherever the source is rendered -- the preview
   pane, a projector, the recording and the stream (this is what the camera's
   own web page does). MONITOR keeps them out of the program output and draws
   them only in passes that render this source on their own.

   MONITOR is the default. It is the only choice that cannot leak a box into a
   recording. Its cost is that OBS's standard preview pane shows nothing: that
   pane does not render sources at all when Studio Mode is off, it blits the
   already-rendered program texture (UI/window-basic-main.cpp, RenderMain ->
   obs_render_main_texture_src_color_only), so anything held out of the program
   is held out of that pane too. The passes that DO render the source by itself
   -- and are therefore where monitor-only boxes appear -- are the Properties /
   Filters / Interaction dialog previews, a Windowed Projector, and the Studio
   Mode preview pane. */
enum { ZC_AI_OVERLAY_PROGRAM = 0, ZC_AI_OVERLAY_MONITOR = 1 };

/* macOS zero-copy. With ENABLE_ZERO_COPY the macOS source becomes an ordinary
   OBS_SOURCE_VIDEO source: the native VideoToolbox session decodes straight
   into a BGRA IOSurface, gs binds it as a GL_TEXTURE_RECTANGLE, and
   video_render draws it — the same shape as the Windows path, no CPU copy and
   no per-frame upload. Without ENABLE_ZERO_COPY the source stays on the
   asynchronous CPU-frame path (obs_source_output_video2), which is also the
   only path that keeps OBS's native 10-bit / HDR handling; a zero-copy build
   renders 10-bit streams as 8-bit. */
#if defined(__APPLE__) && defined(ENABLE_ZERO_COPY)
#define ZC_MAC_ZERO_COPY 1
#endif

#define SYNC_INTERNAL 0
#define SYNC_SSP_TIMESTAMP 1

/* How long a live connection may deliver no video at all before the source is
   rebuilt (see ZcSspSource::watchdog). The stream is silent between frames, so
   this has to be far longer than a frame interval: even a 1 fps camera is only
   silent for a second. */
#define ZC_STALL_TIMEOUT_MS 10000

namespace zc {

/* RAII guard for a graphics section. obs_enter_graphics()/obs_leave_graphics()
   are ref-counted and safe to nest, so this is correct whether or not the
   caller (libobs's render loop) already entered the context. Holding it across
   the whole of videoRender() makes the render thread own the graphics context
   for the entire function, which is what stops it from deadlocking against the
   receive thread's graphics -> decoderMutex_ order. */
struct GraphicsScope {
	GraphicsScope() { obs_enter_graphics(); }
	~GraphicsScope() { obs_leave_graphics(); }
	GraphicsScope(const GraphicsScope &) = delete;
	GraphicsScope &operator=(const GraphicsScope &) = delete;
};

class ZcSspSource : public zc::OverlayTarget {
public:
	/* `async` selects the output mode. Render mode (default) is the existing
	   GPU source: video_render imports hardware surfaces. Async mode is the
	   10-bit/HDR source: it forces the CPU decoder and feeds native
	   P010/I010 obs_source_frame2 frames to obs_source_output_video2, so
	   OBS applies its P010 + HDR (PQ/HLG) GPU conversion. */
	explicit ZcSspSource(obs_source_t *source, bool async = false);
	~ZcSspSource();

	/* zc::OverlayTarget — what the dock pushes down so the Interact window
	   can paint the detection rectangle and the click crosshair. */
	void setOverlayInteraction(zc::OverlayInteraction mode) override;
	void setOverlayMask(bool enabled, float left, float top, float right,
			    float bottom) override;
	void setOverlayMarker(bool on, float nx, float ny) override;
	void setOverlayTrackBox(bool enabled, float left, float top,
				float width, float height) override;

	/* Delivered from the Interact window's mouse callbacks (zc_source.cpp's
	   zc_source_mouse_click/mouse_move). Runs on the OBS UI thread. */
	void onPointerEvent(int phase, int button, int px, int py);

	void update(obs_data_t *settings);
	void activate();
	void deactivate();
	void videoRender(gs_effect_t *effect);
	void videoTick(float seconds);
	void enumActiveSources(obs_source_enum_proc_t cb, void *param);
	uint32_t getWidth();
	uint32_t getHeight();

	/* The camera address this source targets, as a Qt string. Read-only: the
	   dock's Properties path uses it to select the same camera (bug 27). */
	QString ip() const { return QString::fromStdString(ip_); }
	/* The obs_source_t this object is the private data of. Needed by the
	   Properties callback, which is handed THIS object, not the source: reading
	   the settings object means going through the handle (bugs 183/196 — those
	   calls used to cast this pointer to obs_source_t and crashed). */
	obs_source_t *handle() const { return source_; }

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

#ifdef ENABLE_CAMERA_CONTROL
	/* AI detection overlay (Properties checkbox). startAiFeed() opens the
	   camera's own notification socket and keeps the latest detection in
	   aiBoxes_; stopAiFeed() closes it; drawAiOverlay() paints it. All three
	   run on the OBS UI thread except drawAiOverlay(), which runs on the
	   graphics thread and only takes aiMutex_. */
	void setAiOverlay(bool on);
	void startAiFeed();
	void stopAiFeed();
	void drawAiOverlay(gs_effect_t *effect);
	/* The detection rectangle and the click crosshair, painted while the dock
	   has an interaction mode armed. Separate from drawAiOverlay(): this is an
	   editing aid the operator needs whether or not boxes are being drawn,
	   and it carries its own master switch (the mode). */
	void drawInteractionOverlay(gs_effect_t *effect);
#endif

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
	/* Serializes join()/reassign of reconnectThread_ between stop() (OBS thread)
	   and onDisconnected() (the graphics thread, via watchdog()): a concurrent
	   double-join, or assigning over a still-joinable thread, is undefined
	   behaviour (and a joinable reassign calls std::terminate()). */
	std::mutex reconnectMutex_;
	std::atomic<bool> reconnectJoined_{true};
	std::mutex cvMutex_;
	std::condition_variable cv_;
	/* Written by stop() under cvMutex_ and read by the reconnect thread both
	   under that lock and outside it, so it must be atomic. */
	std::atomic<bool> stopping_{false};

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
	/* Written by the decode thread, read by the graphics thread (repeat
	   render) and by OBS's get_width/get_height: atomic, not plain. */
	std::atomic<uint32_t> lastWidth_{0}, lastHeight_{0};

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
	bool audioDecFailLogged_ = false;
#endif

#ifdef ENABLE_CAMERA_CONTROL
	/* AI detection overlay (see setAiOverlay / drawAiOverlay).
	   aiNotifier_ lives on aiThread_, its own QThread, not on the thread that
	   happened to call update(): the notifier owns a socket and two QTimers,
	   which need a thread with a Qt event loop, and OBS does not guarantee that
	   about the update() caller (see startAiFeed).

	   aiOverlay_ is the whole overlay's master switch and is written by the
	   thread that applies settings while drawAiOverlay() reads it on the
	   graphics thread, so it is atomic: unticking "Draw AI box" must stop the
	   boxes on the next frame, not eventually. */
	std::atomic<bool> aiOverlay_{false};
	/* Where a box is allowed to appear. Monitor-only keeps it out of the
	   program output -- and therefore out of the recording and the stream --
	   but that also hides it from the standard preview pane, which just blits
	   the program mix; the *only* way to see boxes in the preview is to draw
	   into the program as well, which is what the camera's own web page does
	   (it paints them over the live view). Chosen in Properties. Written by
	   the update thread, read by the graphics thread. */
	std::atomic<bool> aiInProgram_{false};
	ZcWsNotifier *aiNotifier_ = nullptr;
	QThread *aiThread_ = nullptr;
	/* The host aiNotifier_ was opened for, so re-applying the same settings
	   does not tear the socket down and re-open it. */
	QString aiFeedHost_;
	/* One log line for the first detection the feed delivers, so a "no boxes
	   on screen" report can be told apart from "no detection arriving". */
	std::atomic<bool> aiFirstEventLogged_{false};

	/* --- Interact-window pointer interaction -------------------------------
	   The dock arms one mode at a time; None leaves the overlay read-only.
	   The mode is atomic because the dock writes it from the UI thread while
	   the graphics thread reads it in drawInteractionOverlay(). */
	std::atomic<int> overlayMode_{
		(int)zc::OverlayInteraction::None};
	/* The rectangle and crosshair the dock pushes down, plus the drag the
	   source is running. Both sides live on the OBS UI thread, so the mutex
	   is only there for the graphics thread's reads. */
	std::mutex overlayMutex_;
	bool overlayMaskOn_ = false;
	float overlayMaskL_ = 0.0f, overlayMaskT_ = 0.0f;
	float overlayMaskR_ = 1.0f, overlayMaskB_ = 1.0f;
	bool overlayMarkerOn_ = false;
	float overlayMarkerX_ = 0.5f, overlayMarkerY_ = 0.5f;
	/* The tracking-box ("range") frame the dock mirrors from the camera
	   (bug list 17): amber outline, drawn while the box is enabled. */
	bool overlayTrackOn_ = false;
	float overlayTrackL_ = 0.0f, overlayTrackT_ = 0.0f;
	float overlayTrackW_ = 0.0f, overlayTrackH_ = 0.0f;
	/* The left-button drag in progress, if any: press records the anchor,
	   move follows, release ends it. The rectangle that was on the picture
	   before the press is kept so a click without a drag can put it back. */
	bool overlayDragging_ = false;
	bool overlayDragPrevOn_ = false;
	float overlayDragPrevL_ = 0.0f, overlayDragPrevT_ = 0.0f;
	float overlayDragPrevR_ = 1.0f, overlayDragPrevB_ = 1.0f;
	/* Which gesture the current press started (zc::MaskGesture): a new
	   rectangle, a move of the whole one, or a resize from one corner
	   (bug 212). */
	int overlayDragGesture_ = 0;
	/* One-shot latch for the "nothing to draw" line: reported when the
	   overlay goes empty, cleared as soon as anything is drawn. */
	bool overlayDiagReported_ = false;
	/* What the last "drawn" line described, so that line is emitted when the
	   picture changes rather than on a timer. */
	float overlayDrawL_ = -1.0f, overlayDrawT_ = -1.0f;
	float overlayDrawR_ = -1.0f, overlayDrawB_ = -1.0f;
	int overlayDrawMode_ = -1;
	uint64_t overlayDiagNextNs_ = 0;
	uint64_t overlayRenderNextNs_ = 0;
	uint64_t overlayEnterNextNs_ = 0;
	uint64_t overlayCallNextNs_ = 0;
	float overlayDragAnchorX_ = 0.0f, overlayDragAnchorY_ = 0.0f;
	/* 1x1 solid texture the rectangle and crosshair are stretched from, in
	   the interaction colour (the box texture is red). Graphics thread only. */
	gs_texture_t *overlaySolid_ = nullptr;
	/* 1x1 50% black, for the band the editor dims everything outside the
	   rectangle with (the page does the same). Graphics thread only. */
	gs_texture_t *overlayDim_ = nullptr;
	/* The page's edit accent (#ff4e05, the colour of its corner handles),
	   used for the rectangle while it is being placed. Graphics thread only. */
	gs_texture_t *overlayAccent_ = nullptr;
	/* Same for the tracking-box frame, in amber. Graphics thread only. */
	gs_texture_t *trackSolid_ = nullptr;
	/* The camera address the source is registered under, so a re-registration
	   after an address change can drop the old key. */
	QString overlayRegisteredIp_;

	/* The most recent detection, written by the notifier (UI thread) and read
	   by the graphics thread. aiStampNs_ is when it arrived: a camera that
	   stops sending stops being drawn, instead of freezing its last box on
	   screen forever. */
	std::mutex aiMutex_;
	QVector<ZcAiBox> aiBoxes_;
	uint64_t aiStampNs_ = 0;
	/* 1x1 solid texture the box edges are stretched from. Created and
	   destroyed on the graphics thread only, exactly like cpuTex_. */
	gs_texture_t *aiSolid_ = nullptr;
	/* Diagnostics, graphics thread only. Paced by TIME, not by a line count:
	   the first frames of a fresh source land within milliseconds and a
	   six-line budget was spent before the notification feed had connected and
	   its first `AI` message arrived — so the one line that explains a
	   "no boxes on screen" report was never written (bug report "I enabled the
	   camera's detection and the boxes still did not show", with a log that
	   only held six "no detection received yet" lines).

	   The canvas-mix gate in drawAiOverlay() must stay ABOVE the first diag()
	   call: libobs renders that pass on every frame, and if it ever spends the
	   pacing budget the log fills with mix-sized "nothing drawn" lines that
	   both say nothing and crowd out whatever a real preview pass reports. */
	uint64_t aiDiagNextNs_ = 0;
	/* Whether the "suppressed because this pass is the canvas mix" explanation
	   has been given; it is a design note, not a per-frame condition. */
	bool aiMixExplained_ = false;
	/* One line the first time the camera reports detections that carry no box,
	   so "the feed works but nobody is in frame" can be told apart from "no
	   detection arriving at all". */
	std::atomic<bool> aiEmptyLogged_{false};
	/* One line the first time a box is drawn into EACH distinct pass, naming
	   that pass's size. Nothing else logs a success, so without this a "the
	   boxes never appear" report cannot be told apart from "the boxes are
	   being drawn into a pane the reporter is not looking at" — and a
	   once-ever line was not enough to tell them apart either: it was spent on
	   the first pass of the session, which is always the program mix, leaving
	   every later pass (the Properties preview, a projector window, the
	   Studio Mode pane) silent. Guarded by aiMutex_ and bounded. */
	QSet<quint64> aiLoggedPasses_;
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
	/* Drop the dock's handle on this object before anything is torn down: a
	   click arriving between here and the end of the destructor would
	   otherwise reach a half-destroyed source. */
	zc::overlayUnregisterTarget(overlayRegisteredIp_, this);
	overlayRegisteredIp_.clear();

	stop();
	/* The CPU texture is only ever touched on the graphics thread; the source
	   destructor runs after OBS has stopped rendering it, so this is the one
	   place besides videoRender() where cpuTex_ may be destroyed. */
	obs_enter_graphics();
	if (cpuTex_) {
		gs_texture_destroy(cpuTex_);
		cpuTex_ = nullptr;
	}
#ifdef ENABLE_CAMERA_CONTROL
	/* Same rule for the overlay's solid colour (drawAiOverlay() creates it on
	   the graphics thread). stop() already closed the notification socket. */
	if (aiSolid_) {
		gs_texture_destroy(aiSolid_);
		aiSolid_ = nullptr;
	}
	if (overlaySolid_) {
		gs_texture_destroy(overlaySolid_);
		overlaySolid_ = nullptr;
	}
	if (overlayDim_) {
		gs_texture_destroy(overlayDim_);
		overlayDim_ = nullptr;
	}
	if (overlayAccent_) {
		gs_texture_destroy(overlayAccent_);
		overlayAccent_ = nullptr;
	}
	if (trackSolid_) {
		gs_texture_destroy(trackSolid_);
		trackSolid_ = nullptr;
	}
#endif
	obs_leave_graphics();
}

/* Source-IP validation (bug list "no IP format validation on the source-IP
   box"). Accepts a dotted-quad IPv4 address or a DNS-style host name; anything
   else (empty labels, spaces, a pasted URL) is rejected so a typo cannot be
   applied and left behind as a permanently black source. */
static bool isValidHostAddress(const std::string &host)
{
	if (host.empty() || host.size() > 253)
		return false;

	/* Dotted-quad IPv4: exactly four 0..255 octets. */
	bool quad = true;
	{
		size_t i = 0;
		int octets = 0;
		while (i <= host.size()) {
			size_t j = host.find('.', i);
			if (j == std::string::npos)
				j = host.size();
			if (j == i || j - i > 3) {
				quad = false;
				break;
			}
			int v = 0;
			for (size_t k = i; k < j; ++k) {
				if (host[k] < '0' || host[k] > '9') {
					quad = false;
					break;
				}
				v = v * 10 + (host[k] - '0');
			}
			if (!quad || v > 255)
				break;
			++octets;
			if (j == host.size())
				break;
			i = j + 1;
		}
		if (quad && octets == 4)
			return true;
	}

	/* Host name: letters, digits, '-' and '.', no empty label and no leading or
	   trailing separator. */
	if (host.front() == '.' || host.back() == '.' || host.front() == '-' ||
	    host.back() == '-')
		return false;
	bool prevDot = false;
	for (char c : host) {
		if (c == '.') {
			if (prevDot)
				return false;
			prevDot = true;
			continue;
		}
		const bool alnum = (c >= 'a' && c <= 'z') ||
				   (c >= 'A' && c <= 'Z') ||
				   (c >= '0' && c <= '9');
		if (!alnum && c != '-')
			return false;
		prevDot = false;
	}
	return true;
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
	const std::string candidate = full.substr(prefix.size());
	return isValidHostAddress(candidate) ? candidate : std::string();
}

/* The dock names its sources "ZCamera <ip>", and the rail keys both presence and
   "already added" off that name. A source the operator configured from OBS's own
   Properties dialog keeps the default "ZCamera" name, so the rail cannot match it
   to the IP in its settings, and "Restore Defaults" then wipes the address with
   nothing left to recover it from (bug list: "source name is not synced to
   ZCamera + IP"). Adopt the convention as soon as the address is known. Only the
   bare default is renamed: a name that already carries an address (or a
   duplicate OBS made unique) is left alone, so this can never churn. */
static void syncSourceNameToIp(obs_source_t *source, const std::string &ip)
{
	if (!source || ip.empty())
		return;
	const char *cur = obs_source_get_name(source);
	if (!cur)
		return;
	const std::string name(cur);
	/* The bare default ("ZCamera"), OBS's deduplicated form ("ZCamera 2")
	   and a name the convention wrote earlier ("ZCamera <ip/host>", which
	   always carries a dot) all follow the address; anything else is an
	   operator's own name and is never touched (bug list 75). */
	static const QRegularExpression convention(
		QStringLiteral("^ZCamera(?: \\d+"
			       "| [A-Za-z0-9.-]*\\.[A-Za-z0-9.-]+)?$"));
	if (!convention.match(QString::fromStdString(name)).hasMatch())
		return;
	const std::string want = "ZCamera " + ip;
	if (name == want)
		return;
	obs_source_set_name(source, want.c_str());
}

void ZcSspSource::update(obs_data_t *settings)
{
	std::string ip = obs_data_get_string(settings, PROP_SOURCE_IP);
	/* Refuse a syntactically invalid address outright: the source-IP box has no
	   validation of its own, and an accepted typo would otherwise be applied and
	   surface as a black source (bug list "source IP input has no validation").
	   The address the source already had is kept, not cleared: clearing it let
	   the source fall back to the name and left a row behind in the dock, so a
	   typo could both break a working source and add a phantom camera (bug
	   173). A brand-new source with a typo simply keeps no address, which is
	   what leaves it out of the dock's list. */
	if (!ip.empty() && !isValidHostAddress(ip)) {
		blog(LOG_WARNING,
		     "[obs-zcamera] ignoring invalid source IP '%s'", ip.c_str());
		ip = ip_;
	}
	/* A source saved with an empty address recovers it from its own name, so a
	   scene collection from before the get_defaults fix still renders. Applied
	   before the comparison below, so re-applying unchanged settings is still a
	   no-op and does not restart a healthy stream. */
	if (ip.empty())
		ip = ipFromSourceName(source_);
	/* Adopt the "ZCamera <ip>" convention when the address was typed into the
	   Properties dialog (see the helper). Done before the early return so the
	   name is fixed even when the pipeline settings did not change. */
	syncSourceNameToIp(source_, ip);
	enum zc_hw_backend backend = (enum zc_hw_backend)obs_data_get_int(
		settings, PROP_DECODE_BACKEND);
	/* CUDA/NVDEC is no longer offered in the UI (it renders green in OBS), but
	   a profile that saved backend=1 before the option was removed would still
	   ask for it here. Coerce it back to Auto so the broken path cannot be
	   re-enabled silently from an old profile. */
	if (backend == ZC_HW_CUDA)
		backend = ZC_HW_AUTO;
	int syncMode = (int)obs_data_get_int(settings, PROP_SYNC_MODE);
	bool waitI = obs_data_get_bool(settings, PROP_WAIT_I_FRAME);
	bool lowNoise = obs_data_get_bool(settings, PROP_LOW_NOISE);

	bool changed = (ip != ip_) || (backend != requestedBackend_) ||
		       (syncMode != syncMode_) ||
		       (waitI != waitIFrame_) || (lowNoise != lowNoise_);
	if (changed) {
		stop();
		ip_ = ip;
		/* Re-register the interaction target under the new address: the dock
		   reaches this object by the camera it is driving. A source whose
		   address is cleared is dropped from the map. */
		const QString newIp = QString::fromStdString(ip);
		if (overlayRegisteredIp_ != newIp) {
			zc::overlayUnregisterTarget(overlayRegisteredIp_, this);
			overlayRegisteredIp_ = newIp;
			if (!overlayRegisteredIp_.isEmpty())
				zc::overlayRegisterTarget(overlayRegisteredIp_, this);
		}
		{
			std::lock_guard<std::mutex> lock(decoderMutex_);
			requestedBackend_ = backend;
		}
		syncMode_ = syncMode;
		waitIFrame_ = waitI;
		lowNoise_ = lowNoise;
		start();
	}
#ifdef ENABLE_CAMERA_CONTROL
	/* The AI overlay is a display-only setting: it never restarts the stream
	   and never writes to the camera, it only opens or closes this source's
	   notification socket. Read after the pipeline handling so an IP change in
	   the same Apply — which did stop()/start() — re-opens the feed for the new
	   camera instead of leaving it pointed at the old one. */
	setAiOverlay(obs_data_get_bool(settings, PROP_AI_OVERLAY));
	/* Where the boxes may go. Read here rather than in drawAiOverlay() so the
	   render path only ever reads an already-built value, and so a change in
	   the Properties dialog takes effect on the next frame without touching
	   the notification feed.

	   The default is seeded first, because obs_data_get_int() answers 0 for a
	   key it has never seen and 0 is PROGRAM - the one value that puts the
	   boxes into the recording. A settings blob that never carried this key
	   (a source created straight from the dock's "add to scene", an instance
	   whose settings were cleared) must land on the safe side instead. */
	if (settings)
		obs_data_set_default_int(settings, PROP_AI_OVERLAY_MODE,
					 ZC_AI_OVERLAY_MONITOR);
	aiInProgram_.store(obs_data_get_int(settings, PROP_AI_OVERLAY_MODE) !=
			       ZC_AI_OVERLAY_MONITOR,
			   std::memory_order_relaxed);
#endif
}

#ifdef ENABLE_CAMERA_CONTROL
/* A detection stops being drawn this long after the last `AI` message. The
   camera pushes about 10 Hz while its AI is running, so this only bites when
   the feed goes quiet (the camera rebooted, its AI was switched off, the
   socket dropped): the boxes then disappear instead of freezing on screen. */
static constexpr uint64_t ZC_AI_STALE_NS = 1500000000ULL;

/* The size a camera source reports before its first frame arrives. Every Z
   CAM this plugin drives streams 1280x720 by default; the exact number only
   has to be a sane quad so OBS has something for the mouse to land on and
   the overlay has a pixel space to draw in. */
static constexpr uint32_t kDefaultStreamWidth = 1280;
static constexpr uint32_t kDefaultStreamHeight = 720;

/* Apply the Properties "AI detection overlay" checkbox. Nothing here touches
   the SSP video path or writes to the camera, so the overlay can be toggled
   without restarting a healthy stream. */
void ZcSspSource::setAiOverlay(bool on)
{
	aiOverlay_ = on;
	if (!on) {
		stopAiFeed();
		return;
	}
	/* startAiFeed() is a no-op when a feed for this exact camera is already
	   up, so re-applying unrelated settings costs nothing; and because
	   stop() closed the feed with the connection, this is also what re-opens
	   it after a pipeline change re-started the source. */
	startAiFeed();
}

void ZcSspSource::startAiFeed()
{
#if defined(__APPLE__) && !defined(ZC_MAC_ZERO_COPY)
	/* Nothing to draw on: without a zero-copy build the macOS source is an
	   ASYNC_VIDEO source, so OBS renders its frames itself and the plugin has
	   no video_render hook to overlay onto (see create_ssp_source_info). The
	   Properties checkbox is not offered there either; this guard also covers a
	   profile carried over from Windows with the option saved on. */
	return;
#endif
	if (!aiOverlay_)
		return;
	if (ip_.empty() || !isValidHostAddress(ip_)) {
		/* The overlay keys on the Properties IP field, and a source can exist
		   with it empty (its name carries the address instead, which the video
		   path recovers but this does not). Say so: without this line the
		   checkbox just looks broken, with nothing in the log to follow up. */
		blog(LOG_WARNING,
		     "[obs-zcamera] AI detection overlay is on but this source has no "
		     "camera address (%s): nothing to listen to",
		     ip_.empty() ? "empty" : ip_.c_str());
		return;
	}

	const QString host = QString::fromStdString(ip_);
	if (aiNotifier_ && aiFeedHost_ == host)
		return;
	stopAiFeed();

	/* The notification socket is opened directly rather than through
	   ZcCameraClient: the camera answers the port-81 upgrade whether or not
	   its control API is authenticated (measured on e2ptz sw 1.0.3), so the
	   overlay works for a source whose camera was never logged in from the
	   dock, and — unlike a ZcCameraClient — opening it issues no command at
	   all (a client connect() also puts the camera into rec standby).

	   The notifier gets a thread of its own. It owns a socket and two QTimers,
	   so it needs a thread with a Qt event loop, and update() — the only caller
	   here — is not guaranteed to arrive on one: measured on OBS 31, turning the
	   overlay on from the Properties dialog logged
	   "QObject::startTimer: Timers can only be used with threads started with
	   QThread" twice and the upgrade never completed, so no detection ever
	   reached the source and nothing was drawn. */
	aiThread_ = new QThread;
	aiThread_->setObjectName(QStringLiteral("zc-ai-feed"));
	aiNotifier_ = new ZcWsNotifier;
	aiNotifier_->moveToThread(aiThread_);
	QObject::connect(aiNotifier_, &ZcWsNotifier::eventReceived, aiNotifier_,
			 [this, host](const QString &what, const QJsonObject &payload) {
				 if (what != QLatin1String(ws_events::kAiDetect))
					 return;
				 /* AI feed thread. Only the decoded box list and its
				    arrival stamp cross to the graphics thread. */
				 const QVector<ZcAiBox> boxes = parseAiDetection(payload);
				 const uint64_t now = os_gettime_ns();
				 {
					 std::lock_guard<std::mutex> lock(aiMutex_);
					 aiBoxes_ = boxes;
					 aiStampNs_ = now;
				 }
				 /* The first event that carries something is the one
				    worth logging: an empty room still sends events, and
				    "0 box(es)" says nothing about whether the drawing
				    path works. The coordinates go with the line so a
				    box in the wrong place can be pinned against the
				    camera's own numbers. */
				 if (!boxes.isEmpty() && !aiFirstEventLogged_.exchange(true)) {
					 QString where;
					 for (int i = 0; i < boxes.size() && i < 3; ++i)
						 where += QStringLiteral(" [x%1 y%2 w%3 h%4]")
							 .arg(boxes[i].x, 0, 'f', 3)
							 .arg(boxes[i].y, 0, 'f', 3)
							 .arg(boxes[i].width, 0, 'f', 3)
							 .arg(boxes[i].height, 0, 'f', 3);
					 blog(LOG_INFO,
					      "[obs-zcamera] AI detection overlay: first "
					      "detection from %s, %d box(es)%s",
					      host.toUtf8().constData(), (int)boxes.size(),
					      where.toUtf8().constData());
				 }
				 /* The other half of that report, said once: events are
				    arriving, so the socket, the port-81 upgrade and the
				    camera's own detection are all alive, and the only
				    reason nothing is drawn is that no target is in
				    frame. Without this line a room with no face in it
				    and a dead overlay look identical in the log. */
				 if (boxes.isEmpty() && !aiEmptyLogged_.exchange(true)) {
					 blog(LOG_INFO,
					      "[obs-zcamera] AI detection overlay: %s is "
					      "sending detection events but they carry no "
					      "box (nothing in frame), so the camera's "
					      "detection is running and the overlay simply "
					      "has nothing to draw yet",
					      host.toUtf8().constData());
				 }
			 });
	/* start() opens the socket and arms the timers, so it has to run on the feed
	   thread; QThread::started is delivered there. */
	QObject::connect(aiThread_, &QThread::started, aiNotifier_,
			 [notifier = aiNotifier_, host] { notifier->start(host, false); });
	aiFeedHost_ = host;
	aiThread_->start();
	blog(LOG_INFO, "[obs-zcamera] AI detection overlay: listening to %s notifications",
	     host.toUtf8().constData());
}

void ZcSspSource::stopAiFeed()
{
	if (aiNotifier_) {
		/* stop() closes the socket and disarms the reconnect timer, and both
		   belong to the feed thread — so run it there, and block until it
		   has: the socket is really gone when this returns, which is what
		   re-pointing a source at another camera relies on. */
		ZcWsNotifier *notifier = aiNotifier_;
		aiNotifier_ = nullptr;
		if (aiThread_ && aiThread_->isRunning()) {
			QMetaObject::invokeMethod(
			    notifier,
			    [notifier] {
				    notifier->stop();
				    notifier->deleteLater();
			    },
			    Qt::BlockingQueuedConnection);
		} else {
			/* The feed thread is already down: nothing pumps this object
			   any more, so tear it down here instead. */
			notifier->stop();
			delete notifier;
		}
	}
	if (aiThread_) {
		/* The blocking call above already ran, so the notifier's
		   deleteLater() is queued on that loop and QThread's own cleanup
		   performs it as the thread finishes. */
		aiThread_->quit();
		aiThread_->wait();
		delete aiThread_;
		aiThread_ = nullptr;
	}
	aiFeedHost_.clear();
	/* Re-arm the one-shot log lines: the next feed may be a different camera,
	   and "first detection" / "detections with no box" are worth hearing
	   again for it. Only the atomics are touched here — the pacing and
	   mix-explanation members belong to the graphics thread and are left
	   alone (they are diagnostics, and letting them keep their pace across a
	   feed restart only ever costs a line, never correctness). */
	aiFirstEventLogged_.store(false);
	aiEmptyLogged_.store(false);
	std::lock_guard<std::mutex> lock(aiMutex_);
	aiBoxes_.clear();
	aiStampNs_ = 0;
}

/* Whether the render target currently bound is one of libobs's canvas mixes —
   the pass that fills the texture program, preview, recording and streaming
   all read from. libobs offers a plugin no "am I rendering a preview" flag, so
   the mix is recognised by what it is: an off-screen target the size of the
   canvas, drawn into whole (obs-video.c binds video->render_texture before it
   renders the main mix).

   Every preview path fails that test, which is what makes this work:
   a display — the Properties / Filters / Interaction dialog panes, a projector
   window, the main preview pane — draws straight into its own swapchain
   (obs-display.c render_display_begin() calls gs_load_swapchain()), where
   gs_get_render_target() is NULL; and the main preview pane in normal mode
   does not render sources at all, it blits the mix texture. Skipping the mix
   is therefore what keeps this overlay out of recordings and streams while
   still showing it where the operator frames the shot. The size + viewport
   test is the secondary guard: it also catches a source rendered into an
   off-screen canvas-sized texture by something other than the mix. */
static bool zc_render_target_is_canvas_mix()
{
	gs_texture_t *target = gs_get_render_target();
	if (!target)
		return false;

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return false;
	if (gs_texture_get_width(target) != ovi.base_width ||
	    gs_texture_get_height(target) != ovi.base_height)
		return false;

	gs_rect vp;
	gs_get_viewport(&vp);
	return vp.x <= 0 && vp.y <= 0 && (uint32_t)vp.cx >= ovi.base_width &&
	       (uint32_t)vp.cy >= ovi.base_height;
}

void ZcSspSource::drawAiOverlay(gs_effect_t *effect)
{
	/* The Properties checkbox switches the whole overlay. Tested first, so
	   unticking "Draw AI box" stops the boxes on the very next frame: the
	   feed stops as well (setAiOverlay -> stopAiFeed), but that alone would
	   leave the last detection painted until the staleness timer ran out --
	   and any later refresh would paint it again. Nothing below can draw a
	   box while this is false. */
	if (!aiOverlay_.load(std::memory_order_relaxed))
		return;

	/* Which gate stopped the overlay, at most one line every five seconds and
	   only when the gate is reached: the target named here is the one the
	   source is being drawn into, which is what tells a mix-suppression apart
	   from a preview that simply has no data. */
	auto diag = [this](const char *why) {
		const uint64_t now = os_gettime_ns();
		if (now < aiDiagNextNs_)
			return;
		aiDiagNextNs_ = now + 5000000000ULL;
		gs_texture_t *t = gs_get_render_target();
		blog(LOG_INFO,
		     "[obs-zcamera] AI overlay: nothing drawn (%s) into %s%ux%u",
		     why, t ? "off-screen target " : "a display, ",
		     t ? gs_texture_get_width(t) : 0, t ? gs_texture_get_height(t) : 0);
	};

	/* The canvas mix is tested BEFORE anything else, and returns without
	   spending the budget above. libobs renders this pass every frame, and the
	   recording, the stream and the main preview pane all read it, so it is
	   the one pass that is *supposed* to be empty. Letting it reach diag()
	   first made the log a stream of "nothing drawn ... into off-screen target
	   1920x1080" — the mix, at the canvas size — that both said nothing and
	   used up the one line every five seconds, hiding whatever a preview pass
	   had to report. (Seen live: a "1 box(es) are ready, but ... canvas mix"
	   note followed immediately by three mix-sized "nothing drawn" lines,
	   while the actual preview render — the one that draws — never got a
	   word in.) */
	if (!aiInProgram_.load(std::memory_order_relaxed) &&
	    zc_render_target_is_canvas_mix()) {
		if (!aiMixExplained_) {
			int ready = 0;
			{
				std::lock_guard<std::mutex> lock(aiMutex_);
				ready = (int)aiBoxes_.size();
			}
			if (ready > 0) {
				aiMixExplained_ = true;
				blog(LOG_INFO,
				     "[obs-zcamera] AI overlay: %d box(es) are ready, but "
				     "this render pass is the program canvas mix and they "
				     "are left out of it by design (the stream and the "
				     "recording encode this pass). The standard preview "
				     "pane only blits the mix, so it cannot show them "
				     "either - watch them in the source's Properties, "
				     "Filters or Interaction dialog, in a Windowed "
				     "Projector, or with Studio Mode on.",
				     ready);
			}
		}
		return;
	}

	QVector<ZcAiBox> boxes;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(aiMutex_);
		boxes = aiBoxes_;
		stamp = aiStampNs_;
	}
	if (boxes.isEmpty()) {
		diag("no `AI` message with a box yet - the camera's own "
		     "detection (Face AF or auto framing) must be running, or "
		     "the notification socket is down");
		return;
	}
	if (os_gettime_ns() - stamp > ZC_AI_STALE_NS) {
		diag("the last detection is stale - the camera stopped reporting");
		return;
	}

	/* The camera reports normalized frame coordinates, so they are scaled by
	   the decoded picture size — the same space OBS projects the source into
	   in every preview pane that reaches this code. */
	const uint32_t fw = lastWidth_.load(std::memory_order_relaxed);
	const uint32_t fh = lastHeight_.load(std::memory_order_relaxed);
	if (!fw || !fh) {
		diag("no decoded picture size yet - no frame has been decoded into "
		     "the source's own texture");
		return;
	}

	gs_effect_t *e = effect ? effect : gs_get_effect();
	if (!e) {
		diag("no effect to draw with");
		return;
	}
	gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
	if (!img) {
		diag("the effect has no `image` parameter");
		return;
	}

	if (!aiSolid_) {
		/* A single opaque red pixel; every edge is a stretched sprite of
		   it, so the overlay costs four tiny draws per box and needs no
		   geometry of its own. Created here, on the graphics thread, and
		   released in ~ZcSspSource like cpuTex_. */
		uint8_t red[4] = {255, 0, 0, 255};
		const uint8_t *data = red;
		aiSolid_ = gs_texture_create(1, 1, GS_RGBA, 1, &data, 0);
		if (!aiSolid_)
			return;
	}

	/* Thick enough to see at any source size, thin enough not to hide the
	   person: about 3 px at 1080p, 2 px at 720p, never less than 1. A pass
	   that renders the source smaller than its own frame -- the Properties
	   dialog's preview, a Windowed Projector, the Studio Mode pane -- would
	   otherwise scale a 3 px border down to a fraction of a pixel and lose it
	   entirely, so there the border is grown from the size gs_get_viewport()
	   reports for the pass. In the program mix the viewport is the whole
	   canvas, so the thickness there is unchanged.

	   Written without std::min/std::max because this file includes Windows.h,
	   which brings the min/max macros in and breaks the template form. */
	uint32_t thick_px = (fw < fh ? fw : fh) / 360;
	if (thick_px < 1)
		thick_px = 1;
	{
		gs_rect vp;
		gs_get_viewport(&vp);
		if (vp.cx > 0 && (uint32_t)vp.cx < fw) {
			const float scale = (float)vp.cx / (float)fw;
			if (scale > 0.0f) {
				const float want = 2.0f / scale;
				if ((float)thick_px < want)
					thick_px = (uint32_t)(want + 0.999f);
			}
		}
		/* A pathological viewport (1x1) must not ask for a 2000 px border. */
		if (thick_px > 24)
			thick_px = 24;
	}
	const float thick = (float)thick_px;
	int drawn = 0;

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(img, aiSolid_);

	for (const ZcAiBox &box : boxes) {
		float x = (float)box.x * (float)fw;
		float y = (float)box.y * (float)fh;
		float w = (float)box.width * (float)fw;
		float h = (float)box.height * (float)fh;
		if (w <= 0.0f || h <= 0.0f)
			continue;
		/* A target half out of frame must not draw a box that wraps around
		   the picture. */
		if (x < 0.0f) {
			w += x;
			x = 0.0f;
		}
		if (y < 0.0f) {
			h += y;
			y = 0.0f;
		}
		if (x + w > (float)fw)
			w = (float)fw - x;
		if (y + h > (float)fh)
			h = (float)fh - y;
		if (w <= 0.0f || h <= 0.0f)
			continue;

		if (w <= thick * 2.0f || h <= thick * 2.0f) {
			/* Smaller than its own border: fill it instead of drawing
			   four edges on top of each other. */
			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_draw_sprite(aiSolid_, 0, (uint32_t)w, (uint32_t)h);
			gs_matrix_pop();
			drawn++;
			continue;
		}

		struct Edge {
			float x, y, cx, cy;
		};
		const Edge edges[4] = {
			{x, y, w, thick},
			{x, y + h - thick, w, thick},
			{x, y, thick, h},
			{x + w - thick, y, thick, h},
		};
		for (const Edge &edge : edges) {
			gs_matrix_push();
			gs_matrix_translate3f(edge.x, edge.y, 0.0f);
			gs_draw_sprite(aiSolid_, 0, (uint32_t)edge.cx, (uint32_t)edge.cy);
			gs_matrix_pop();
		}
		drawn++;
	}

	gs_blend_state_pop();

	/* Said once per distinct pass. Nothing else in this file logs a successful
	   draw, so without this line "the boxes never show" and "the boxes are
	   being drawn into a pane you are not looking at" read identically in a
	   log -- which is exactly the report that prompted the scope option above.
	   The pass size is the tell: the program mix is the canvas size, anything
	   else is the Properties dialog's preview, a projector window or the
	   Studio Mode pane. */
	if (drawn > 0) {
		gs_rect vp;
		gs_get_viewport(&vp);
		const quint64 pass = ((quint64)vp.cx << 32) | (quint64)vp.cy;
		bool first = false;
		{
			std::lock_guard<std::mutex> lock(aiMutex_);
			if (!aiLoggedPasses_.contains(pass) &&
			    aiLoggedPasses_.size() < 8) {
				aiLoggedPasses_.insert(pass);
				first = true;
			}
		}
		if (first) {
			blog(LOG_INFO,
			     "[obs-zcamera] AI overlay: drew %d box(es) into a %ux%u pass%s",
			     drawn, (unsigned)vp.cx, (unsigned)vp.cy,
			     aiInProgram_.load(std::memory_order_relaxed)
				 ? ""
				 : " (monitor-only mode, so this pass is not the program)");
		}
	}
}
#endif /* ENABLE_CAMERA_CONTROL */

#ifdef ENABLE_CAMERA_CONTROL
/* ------------------------------------------------------------------ */
/* Interact-window pointer interaction                                 */
/* ------------------------------------------------------------------ */

void ZcSspSource::setOverlayInteraction(zc::OverlayInteraction mode)
{
	overlayMode_.store((int)mode, std::memory_order_relaxed);
	if (mode != zc::OverlayInteraction::MaskEdit) {
		/* Leaving mask-edit ends a drag that was in progress, so a mode
		   switch cannot leave the rectangle half-defined. */
		std::lock_guard<std::mutex> lock(overlayMutex_);
		overlayDragging_ = false;
	}
}

void ZcSspSource::setOverlayMask(bool enabled, float left, float top,
				 float right, float bottom)
{
	std::lock_guard<std::mutex> lock(overlayMutex_);
	overlayMaskOn_ = enabled;
	overlayMaskL_ = left;
	overlayMaskT_ = top;
	overlayMaskR_ = right;
	overlayMaskB_ = bottom;
}

void ZcSspSource::setOverlayMarker(bool on, float nx, float ny)
{
	std::lock_guard<std::mutex> lock(overlayMutex_);
	overlayMarkerOn_ = on;
	overlayMarkerX_ = nx;
	overlayMarkerY_ = ny;
}

void ZcSspSource::setOverlayTrackBox(bool enabled, float left, float top,
				     float width, float height)
{
	std::lock_guard<std::mutex> lock(overlayMutex_);
	overlayTrackOn_ = enabled;
	overlayTrackL_ = left;
	overlayTrackT_ = top;
	overlayTrackW_ = width;
	overlayTrackH_ = height;
}

/* From the Interact window's mouse callbacks. `px`/`py` are in the source's
   own pixel space (libobs maps them for us), which is the same space the
   overlay draws in, so normalising by getWidth()/getHeight() lines the two
   up exactly. */
void ZcSspSource::onPointerEvent(int phase, int button, int px, int py)
{
	const auto mode = (zc::OverlayInteraction)overlayMode_.load(
		std::memory_order_relaxed);
	if (mode == zc::OverlayInteraction::None)
		return;

	const uint32_t w = getWidth();
	const uint32_t h = getHeight();
	if (!w || !h)
		return;
	float nx = (float)px / (float)w;
	float ny = (float)py / (float)h;
	nx = nx < 0.0f ? 0.0f : (nx > 1.0f ? 1.0f : nx);
	ny = ny < 0.0f ? 0.0f : (ny > 1.0f ? 1.0f : ny);

	/* Mask editing is tracked here as well, so the rectangle follows the
	   pointer on the next frame instead of after a camera round-trip. Only
	   the left button draws; the dock is told about every phase regardless,
	   because it is the side that writes the camera.

	   Bug 212: a press used to start a new rectangle wherever it landed, so
	   the first click of an edit threw away the rectangle being edited. The
	   gestures are the camera page's own (corner = resize, inside = move,
	   outside = new), shared with the dock through the bridge so the drawn
	   rectangle and the written one are the same one. */
	if (mode == zc::OverlayInteraction::MaskEdit &&
	    (phase == zc::OverlayPointerDrag || button == MOUSE_LEFT)) {
		std::lock_guard<std::mutex> lock(overlayMutex_);
		if (phase == zc::OverlayPointerPress) {
			/* Remember what was on the picture. A press that never moves
			   is not an edit: the dock refuses to write a zero-area mask,
			   so if this collapsed the rectangle it would stay collapsed
			   for good (nothing writes it back — the camera's next
			   snapshot is the same corner set the dock already has). */
			overlayDragPrevOn_ = overlayMaskOn_;
			overlayDragPrevL_ = overlayMaskL_;
			overlayDragPrevT_ = overlayMaskT_;
			overlayDragPrevR_ = overlayMaskR_;
			overlayDragPrevB_ = overlayMaskB_;
			overlayDragging_ = true;
			overlayDragAnchorX_ = nx;
			overlayDragAnchorY_ = ny;
			overlayDragGesture_ = (int)zc::maskGestureAt(
			    overlayMaskL_, overlayMaskT_, overlayMaskR_,
			    overlayMaskB_, overlayMaskOn_, nx, ny);
			if (overlayDragGesture_ ==
			    (int)zc::MaskGesture::New) {
				/* A fresh rectangle starts as a point under the
				   pointer and grows with the drag. */
				overlayMaskOn_ = true;
				overlayMaskL_ = nx;
				overlayMaskT_ = ny;
				overlayMaskR_ = nx;
				overlayMaskB_ = ny;
			}
		} else if (phase == zc::OverlayPointerDrag && overlayDragging_) {
			float l = overlayMaskL_, t = overlayMaskT_,
			      r = overlayMaskR_, b = overlayMaskB_;
			zc::maskDragRect((zc::MaskGesture)overlayDragGesture_,
					 overlayDragPrevL_, overlayDragPrevT_,
					 overlayDragPrevR_, overlayDragPrevB_,
					 overlayDragAnchorX_,
					 overlayDragAnchorY_, nx, ny, &l, &t, &r, &b);
			overlayMaskOn_ = true;
			overlayMaskL_ = l;
			overlayMaskT_ = t;
			overlayMaskR_ = r;
			overlayMaskB_ = b;
		} else if (phase == zc::OverlayPointerRelease) {
			overlayDragging_ = false;
			const float dw = overlayMaskR_ - overlayMaskL_;
			const float dh = overlayMaskB_ - overlayMaskT_;
			if (dw < 0.02f && dh < 0.02f) {
				/* Too small to be a rectangle (a stray click, or a
				   resize dragged shut): put the previous one back. */
				overlayMaskOn_ = overlayDragPrevOn_;
				overlayMaskL_ = overlayDragPrevL_;
				overlayMaskT_ = overlayDragPrevT_;
				overlayMaskR_ = overlayDragPrevR_;
				overlayMaskB_ = overlayDragPrevB_;
			}
		}
	}

	zc::overlayDispatchPointer(phase, button, nx, ny);
}

void ZcSspSource::drawInteractionOverlay(gs_effect_t *effect)
{
	const auto mode = (zc::OverlayInteraction)overlayMode_.load(
		std::memory_order_relaxed);
	/* The rectangle, the click crosshair and the range frame are operator
	   overlays, not detections: with the detection area switched on the
	   rectangle has to stay on the picture whether or not the editor is armed
	   (bug 28 — it used to be drawn only while an interaction mode was up, so
	   the window looked empty and the editor had nothing to grab). Only the
	   corner handles belong to the editor, and they still test `mode` below. */
	bool anyOperatorOverlay = false;
	{
		std::lock_guard<std::mutex> lock(overlayMutex_);
		anyOperatorOverlay = overlayMaskOn_ || overlayMarkerOn_ ||
				     overlayTrackOn_;
	}
	{
		/* Self-test aid (opt-in: only with the harness that also seeds a
		   rectangle): what state this pass sees, and for which camera. */
		const uint64_t now = os_gettime_ns();
		if (qEnvironmentVariableIsSet("ZC_TEST_MASK") &&
		    now >= overlayCallNextNs_) {
			overlayCallNextNs_ = now + 5000000000ULL;
			blog(LOG_INFO,
			     "[obs-zcamera] overlay call: ip=%s mode=%d mask=%d "
			     "marker=%d track=%d",
			     ip_.c_str(), (int)mode, overlayMaskOn_ ? 1 : 0,
			     overlayMarkerOn_ ? 1 : 0, overlayTrackOn_ ? 1 : 0);
		}
	}
	if (mode == zc::OverlayInteraction::None && !anyOperatorOverlay) {
		/* Once per "went empty", not every five seconds: a report of "the
		   frame is not there" can be answered from the log, because this says
		   the source had nothing to draw — a state problem, not a drawing
		   one — without turning a quiet log into a drumbeat. */
		if (!overlayDiagReported_) {
			overlayDiagReported_ = true;
			overlayDiagNextNs_ = 0;
			blog(LOG_INFO,
			     "[obs-zcamera] overlay: no rectangle, marker or box is "
			     "set on this source (mode %d), nothing drawn",
			     (int)mode);
		}
		return;
	}
	overlayDiagReported_ = false;

	/* The editing aid never goes into the program: it is drawn into a preview
	   pass or not at all. Unlike the boxes it has no in-program mode -- it is
	   a cursor, not content. */
	if (zc_render_target_is_canvas_mix())
		return;

	const uint32_t fw = lastWidth_.load(std::memory_order_relaxed)
				? lastWidth_.load(std::memory_order_relaxed)
				: kDefaultStreamWidth;
	const uint32_t fh = lastHeight_.load(std::memory_order_relaxed)
				? lastHeight_.load(std::memory_order_relaxed)
				: kDefaultStreamHeight;
	if (!fw || !fh)
		return;

	gs_effect_t *e = effect ? effect : gs_get_effect();
	if (!e)
		return;
	gs_eparam_t *img = gs_effect_get_param_by_name(e, "image");
	if (!img)
		return;

	if (!overlaySolid_) {
		/* Cyan: distinct from the red detection boxes, so a mask edge is
		   never read as a detection. Created on the graphics thread and
		   released in ~ZcSspSource, exactly like aiSolid_. */
		uint8_t cyan[4] = {0, 229, 255, 255};
		const uint8_t *data = cyan;
		overlaySolid_ = gs_texture_create(1, 1, GS_RGBA, 1, &data, 0);
		if (!overlaySolid_)
			return;
		/* The editor's dimming band (the page's 50% black outside the
		   rectangle). */
		uint8_t dim[4] = {0, 0, 0, 128};
		const uint8_t *dimData = dim;
		overlayDim_ = gs_texture_create(1, 1, GS_RGBA, 1, &dimData, 0);
		/* The page's edit accent, #ff4e05 — the colour it fills the four
		   corner handles with while the detection area is being placed. */
		uint8_t accent[4] = {0xff, 0x4e, 0x05, 0xff};
		const uint8_t *accentData = accent;
		overlayAccent_ = gs_texture_create(1, 1, GS_RGBA, 1, &accentData,
						   0);
	}

	uint32_t thick_px = (fw < fh ? fw : fh) / 360;
	if (thick_px < 1)
		thick_px = 1;
	{
		gs_rect vp;
		gs_get_viewport(&vp);
		if (vp.cx > 0 && (uint32_t)vp.cx < fw) {
			const float scale = (float)vp.cx / (float)fw;
			if (scale > 0.0f) {
				const float want = 2.0f / scale;
				if ((float)thick_px < want)
					thick_px = (uint32_t)(want + 0.999f);
			}
		}
		if (thick_px > 24)
			thick_px = 24;
	}
	const float thick = (float)thick_px;

	bool maskOn = false, markerOn = false, trackOn = false;
	float ml = 0.0f, mt = 0.0f, mr = 1.0f, mb = 1.0f, mx = 0.5f, my = 0.5f;
	float tl = 0.0f, tt = 0.0f, tw = 0.0f, th = 0.0f;
	{
		std::lock_guard<std::mutex> lock(overlayMutex_);
		maskOn = overlayMaskOn_;
		ml = overlayMaskL_;
		mt = overlayMaskT_;
		mr = overlayMaskR_;
		mb = overlayMaskB_;
		markerOn = overlayMarkerOn_;
		mx = overlayMarkerX_;
		my = overlayMarkerY_;
		trackOn = overlayTrackOn_;
		tl = overlayTrackL_;
		tt = overlayTrackT_;
		tw = overlayTrackW_;
		th = overlayTrackH_;
	}

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(img, overlaySolid_);

	/* The colour of the quad comes from the effect's `image` parameter, not
	   from the texture handed to `gs_draw_sprite`: passing a texture there and
	   relying on it silently paints everything in whatever the parameter was
	   last set to (the dim bands came out in the plain overlay colour, bug 28).
	   So the helper sets the parameter itself, every call. */
	const auto fillWith = [&](gs_texture_t *tex, float x, float y, float w,
				  float h) {
		if (!tex || w <= 0.0f || h <= 0.0f)
			return;
		gs_effect_set_texture(img, tex);
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_draw_sprite(tex, 0, (uint32_t)w, (uint32_t)h);
		gs_matrix_pop();
	};
	const auto fill = [&](float x, float y, float w, float h) {
		fillWith(overlaySolid_, x, y, w, h);
	};

	if (maskOn && mr > ml && mb > mt) {
		const float x = ml * (float)fw, y = mt * (float)fh;
		const float w = (mr - ml) * (float)fw, h = (mb - mt) * (float)fh;
		/* Proof for a live check that the frame really reaches the picture:
		   emitted when the rectangle or the mode changes, and it names the
		   surface being drawn into (a display, or a mix nobody would ever
		   see). Silent while nothing changes, so a shipped build logs once
		   per edit and not once per five seconds. */
		{
			const bool changed =
			    std::fabs(ml - overlayDrawL_) > 1e-4f ||
			    std::fabs(mt - overlayDrawT_) > 1e-4f ||
			    std::fabs(mr - overlayDrawR_) > 1e-4f ||
			    std::fabs(mb - overlayDrawB_) > 1e-4f ||
			    (int)mode != overlayDrawMode_;
			if (changed) {
				overlayDrawL_ = ml;
				overlayDrawT_ = mt;
				overlayDrawR_ = mr;
				overlayDrawB_ = mb;
				overlayDrawMode_ = (int)mode;
				gs_texture_t *rt = gs_get_render_target();
				blog(LOG_INFO,
				     "[obs-zcamera] overlay draw: mask rect "
				     "%.3f,%.3f,%.3f,%.3f on %ux%u into %s%ux%u "
				     "mode %d",
				     ml, mt, mr, mb, fw, fh,
				     rt ? "off-screen target " : "a display, ",
				     rt ? gs_texture_get_width(rt) : 0,
				     rt ? gs_texture_get_height(rt) : 0,
				     (int)mode);
			}
		}
		/* Editing dims everything outside the rectangle, exactly as the page
		   does (`_framingDrawMaskArea` fills the canvas with 50% black and
		   clears the rectangle): the area that will be watched is the bright
		   part, and there is no mistaking which rectangle is being dragged.
		   Four bands, because the solid draw here has no "clear" op. The
		   frame itself switches to the page's edit accent (#ff4e05) while it
		   is being placed — the cyan read-only colour reads as "this is the
		   area", the accent reads as "this is the thing you are dragging". */
		const bool editing = (mode == zc::OverlayInteraction::MaskEdit);
		if (editing) {
			fillWith(overlayDim_, 0.0f, 0.0f, (float)fw, y);
			fillWith(overlayDim_, 0.0f, y + h, (float)fw,
				 (float)fh - y - h);
			fillWith(overlayDim_, 0.0f, y, x, h);
			fillWith(overlayDim_, x + w, y, (float)fw - x - w, h);
		}
		gs_texture_t *frame =
		    (editing && overlayAccent_) ? overlayAccent_ : overlaySolid_;
		fillWith(frame, x, y, w, thick);
		fillWith(frame, x, y + h - thick, w, thick);
		fillWith(frame, x, y, thick, h);
		fillWith(frame, x + w - thick, y, thick, h);
		/* Corner handles only while editing, so a read-only view stays
		   clean. */
		if (editing) {
			const float s = thick * 4.0f;
			if (w > s * 2.0f && h > s * 2.0f) {
				fillWith(frame, x, y, s, s);
				fillWith(frame, x + w - s, y, s, s);
				fillWith(frame, x, y + h - s, s, s);
				fillWith(frame, x + w - s, y + h - s, s, s);
			}
		}
	}

	if (markerOn) {
		const float cx = mx * (float)fw, cy = my * (float)fh;
		const float arm = thick * 6.0f;
		fill(cx - arm, cy - thick / 2.0f, arm * 2.0f, thick);
		fill(cx - thick / 2.0f, cy - arm, thick, arm * 2.0f);
	}

	/* The tracking box's own texture, created lazily like the mask's: amber,
	   so the range frame never reads as a red detection box or the cyan
	   detection-area rectangle (bug list 17). */
	if (trackOn && tw > 0.0f && th > 0.0f) {
		if (!trackSolid_) {
			uint8_t amber[4] = {255, 176, 32, 255};
			const uint8_t *data = amber;
			trackSolid_ = gs_texture_create(1, 1, GS_RGBA, 1, &data, 0);
		}
		if (trackSolid_) {
			const float x = tl * (float)fw, y = tt * (float)fh;
			fillWith(trackSolid_, x, y, tw * (float)fw, thick);
			fillWith(trackSolid_, x, y + th * (float)fh - thick,
				 tw * (float)fw, thick);
			fillWith(trackSolid_, x, y, thick, th * (float)fh);
			fillWith(trackSolid_, x + tw * (float)fw - thick, y, thick,
				 th * (float)fh);
		}
	}

	gs_blend_state_pop();
}
#endif /* ENABLE_CAMERA_CONTROL */

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

	client_ = std::make_shared<SSPClientIso>(ip_);

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

#ifdef ENABLE_CAMERA_CONTROL
	/* stop() closed the overlay feed along with the connection, and the
	   reconnect path reaches start() without ever going through update() — so
	   without this the boxes would silently stay gone after the first camera
	   link blip. startAiFeed() does nothing when the overlay is off or a feed
	   for this camera is already up. */
	startAiFeed();
#endif
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

#ifdef ENABLE_CAMERA_CONTROL
	/* The AI overlay belongs to this camera connection as well: closing the
	   source must not leave a notification socket open. Any stale boxes go
	   with it, so a source re-pointed at another camera cannot show the old
	   camera's people for a frame. */
	stopAiFeed();
#endif

	/* Join the reconnect thread so it never touches freed `this`. Done with
	   lifecycleMutex_ released: that thread may be inside reconnectNow(),
	   which takes it. reconnectMutex_ serializes this against onDisconnected()
	   (graphics thread) doing the same join/reassign. */
	{
		std::lock_guard<std::mutex> lk(reconnectMutex_);
		if (reconnectThread_.joinable() &&
		    reconnectThread_.get_id() != std::this_thread::get_id()) {
			reconnectThread_.join();
		}
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
		audioDecFailLogged_ = false;
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
		/* Record what actually got built, success or not: the factory
		   falls back to Software when a hardware create fails, and the
		   status line must not keep advertising the hardware backend
		   that is not decoding. */
		std::lock_guard<std::mutex> lock(decoderMutex_);
		backend_ = dec ? dec->backend : backend;
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
		/* drawn_ and pending_ were produced by the decoder being replaced.
		   The VideoToolbox backend owns the texture it rebinds every frame
		   and destroys it with the decoder (owns_texture is false there),
		   so a cached drawn_ would be a dangling gs_texture on the next
		   render — unlike the FFmpeg backends, whose textures are made per
		   frame and owned by the frame. Drop both first.
		   Lock order stays graphics -> frameMutex_ -> decoderMutex_, the
		   same order videoRender uses. */
		struct zc_decoded_frame stale = {};
		struct zc_decoded_frame last = {};
		obs_enter_graphics();
		{
			std::lock_guard<std::mutex> lock(frameMutex_);
			if (hasPending_) {
				stale = pending_;
				pending_ = {};
				hasPending_ = false;
			}
			if (drawn_.texture || drawn_.avframe) {
				last = drawn_;
				drawn_ = {};
			}
		}
		release_frame_direct(&stale);
		release_frame_direct(&last);
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
	/* Serialize join/reassign with stop(), which does the same from the OBS
	   thread; without this the two can join or reassign reconnectThread_
	   concurrently (UB, and a joinable reassign calls std::terminate()). */
	std::lock_guard<std::mutex> lk(reconnectMutex_);
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
					 [this] { return stopping_.load(); }))
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
	if (zc_frame_is_gpu(frame)) {
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
			/* One line, not one per audio packet (~47/s): the decoder
			   cannot come up by itself, repeating the warning just
			   floods the log. */
			if (!audioDecFailLogged_) {
				audioDecFailLogged_ = true;
				blog(LOG_WARNING,
				     "[obs-zcamera] could not open the AAC decoder; "
				     "the source will have no audio");
			}
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
	{
		/* Self-test aid (opt-in): is this pass running at all, and into
		   what. Only with ZC_TEST_MASK set — a shipped build stays quiet. */
		const uint64_t now = os_gettime_ns();
		if (qEnvironmentVariableIsSet("ZC_TEST_MASK") &&
		    now >= overlayEnterNextNs_) {
			overlayEnterNextNs_ = now + 5000000000ULL;
			gs_texture_t *rt = gs_get_render_target();
			blog(LOG_INFO,
			     "[obs-zcamera] video_render enter, target %s%ux%u",
			     rt ? "off-screen " : "display ",
			     rt ? gs_texture_get_width(rt) : 0,
			     rt ? gs_texture_get_height(rt) : 0);
		}
	}
	gs_texture_t *tex = NULL;
	bool fallbackToSoftware = false;
	int frameW = 0, frameH = 0;
	/* Own the graphics context for the whole render, BEFORE frameMutex_ and
	   decoderMutex_. The receive thread takes graphics -> decoderMutex_ inside
	   decode(); taking decoderMutex_ first here and entering graphics later
	   (inside import()) was the exact opposite order and deadlocked the source. */
	GraphicsScope gfx;
	/* Runs before the early returns below, so a stream that never produces a
	   frame is noticed too. */
	watchdog();
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (hasPending_ && zc_frame_is_gpu(&pending_)) {
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

	if (!tex) {
#ifdef ENABLE_CAMERA_CONTROL
		/* No picture this pass: the stream is still coming up, or it
		   dropped and the source is reconnecting. The operator overlays
		   are quads drawn on the pass — not pixels of the image — so they
		   have no business waiting for a frame. Tying them to the texture
		   is what made a reconnecting camera look like a broken detection
		   area: the window stayed black and the editor had nothing on it to
		   grab. libobs has already entered the Draw pass, so the quads can
		   go on now; there is simply no image underneath them. */
		if (effect)
			drawInteractionOverlay(effect);
#endif
		return;
	}

	/* Draw the texture. */
	const bool rect = gs_texture_is_rect(tex);
	if (effect && !rect) {
		/* libobs already entered the default effect's Draw pass for a normal
		   (non-rectangle) source. */
		gs_effect_set_texture(
			gs_effect_get_param_by_name(effect, "image"), tex);
		gs_draw_sprite(tex, 0, 0, 0);

#ifdef ENABLE_CAMERA_CONTROL
		drawAiOverlay(effect);
		/* The operator overlay (detection rectangle, click crosshair, range
		   frame) has to be drawn on THIS path too. Windows takes it: the
		   texture is a normal sampler2D and libobs already entered the
		   default effect's Draw pass, so only the rect path below used to
		   call the overlay — which is why the rectangle never appeared on a
		   Windows build while the AI boxes did (bug 28). */
		drawInteractionOverlay(effect);
#endif
		return;
	}

	/* A GL_TEXTURE_RECTANGLE cannot be sampled by the normal default
	   effect's sampler2D. macOS zero-copy therefore registers this source as
	   OBS_SOURCE_CUSTOM_DRAW and draws its own pass, selecting the
	   texture_rect variant of the same base effect. */
	gs_effect_t *draw_effect =
		rect ? obs_get_base_effect(OBS_EFFECT_DEFAULT_RECT)
		     : (effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT));
	if (!draw_effect)
		return;

	gs_effect_set_texture(gs_effect_get_param_by_name(draw_effect, "image"),
			      tex);
	{
		/* Self-test aid (opt-in): proves the source's render pass runs at
		   all, and names the surface. */
		const uint64_t now = os_gettime_ns();
		if (qEnvironmentVariableIsSet("ZC_TEST_MASK") &&
		    now >= overlayRenderNextNs_) {
			overlayRenderNextNs_ = now + 5000000000ULL;
			gs_texture_t *rt = gs_get_render_target();
			blog(LOG_INFO,
			     "[obs-zcamera] video_render into %s%ux%u",
			     rt ? "off-screen target " : "a display, ",
			     rt ? gs_texture_get_width(rt) : 0,
			     rt ? gs_texture_get_height(rt) : 0);
		}
	}
	while (gs_effect_loop(draw_effect, "Draw")) {
		gs_draw_sprite(tex, 0, 0, 0);

#ifdef ENABLE_CAMERA_CONTROL
		/* The AI overlay goes on top of the picture, in this source's own
		   pixel space, and only into a preview display — see the helper. */
		drawAiOverlay(draw_effect);
		/* Then the interaction aid (detection rectangle, click crosshair),
		   which is drawn whenever a mode is armed, boxes or not. */
		drawInteractionOverlay(draw_effect);
#endif
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
	/* Before the first frame OBS asks for the size, and 0 makes the source a
	   zero-size quad: the Interact window then has no surface to take the
	   mouse on, and the overlay's normalised coordinates cannot be turned
	   into pixels. A camera that is still reconnecting has to be editable
	   all the same, so fall back to the resolution every Z CAM streams. */
	const uint32_t w = lastWidth_;
	return w ? w : kDefaultStreamWidth;
}

uint32_t ZcSspSource::getHeight()
{
	std::lock_guard<std::mutex> lock(frameMutex_);
	const uint32_t h = lastHeight_;
	return h ? h : kDefaultStreamHeight;
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

static obs_properties_t *zc_source_getproperties(void *data)
{
	/* A ZCamera source is really driven from the control dock (discovery,
	   add-to-scene, settings, PTZ); the classic properties dialog is kept
	   minimal so it cannot shadow that, and opening it (which happens both when
	   the source is added and when Properties is clicked) raises the dock so the
	   operator lands on the surface that controls the camera. The camera that
	   this source represents is passed along so the dock selects it (bug 27). */
	auto *s = static_cast<zc::ZcSspSource *>(data);
	/* Read the address from the SOURCE's settings, not from the live pipeline:
	   a source that has not started (or has just been loaded with a scene
	   collection) has no live address yet, and handing an empty host to the
	   dock left it on whatever camera was selected last — the Properties dialog
	   then showed the wrong camera, and after a restart the control page came up
	   empty until another source was clicked (bugs 149/169). */
	QString host;
	if (s) {
		/* `data` is this plugin's ZcSspSource object (what create() returned),
		   NOT an obs_source_t: the handle has to be taken from it before the
		   settings object can be read. Casting the object to obs_source_t
		   crashed on every double-click of a source (bugs 183/196). */
		obs_data_t *saved = s->handle() ? obs_source_get_settings(s->handle())
						: nullptr;
		if (saved) {
			host = QString::fromUtf8(
			    obs_data_get_string(saved, PROP_SOURCE_IP));
			obs_data_release(saved);
		}
		if (host.isEmpty())
			host = s->ip();
	}
	zc_show_control_dock(host);

	/* The IP box gets live validation (bug list "source IP input has no
	   validation"): update() refuses an invalid address, but silently, so the
	   operator saw OK applied and a black source. The modified callback fires
	   on each edit of the field and paints the verdict into the info row below
	   it; the pinned OBS 31.0 headers have no per-keystroke text-changed
	   callback, so this is the closest live hook. */
	obs_properties_t *props = obs_properties_create();
	auto *ip = obs_properties_add_text(props, PROP_SOURCE_IP,
				obs_module_text("SSPPlugin.SourceProps.SourceIp"),
				OBS_TEXT_DEFAULT);
	auto *ipCheck = obs_properties_add_text(
	    props, "zc_source_ip_check", "", OBS_TEXT_INFO);
	obs_property_set_long_description(
	    ip, obs_module_text("SSPPlugin.SourceProps.SourceIpTip"));
	const auto validateIp = [](obs_properties_t *props,
				   obs_property_t *, obs_data_t *settings) {
		auto *check = obs_properties_get(props, "zc_source_ip_check");
		if (!check || !settings)
			return true;
		const char *value =
		    obs_data_get_string(settings, PROP_SOURCE_IP);
		const bool bad = *value && !isValidHostAddress(value);
		obs_property_set_description(
		    check, bad ? obs_module_text(
					 "SSPPlugin.SourceProps.SourceIpInvalid")
			       : "");
		obs_property_text_set_info_type(
		    check, bad ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
		return true;
	};
	obs_property_set_modified_callback(ip, validateIp);
	validateIp(props, ip, nullptr);
#ifdef ENABLE_CAMERA_CONTROL
	/* Draw what the camera's AI sees over its own picture. Unticking this
	   stops every box on the next frame (drawAiOverlay returns at once).
	   Not offered on a non-zero-copy macOS build, where the source has no
	   render hook to draw on (see ZcSspSource::startAiFeed). */
#if !defined(__APPLE__) || defined(ZC_MAC_ZERO_COPY)
	obs_properties_add_bool(props, PROP_AI_OVERLAY,
				obs_module_text("ZCameraPlugin.SourceProps.AiOverlay"));
	obs_property_set_long_description(
	    obs_properties_get(props, PROP_AI_OVERLAY),
	    obs_module_text("ZCameraPlugin.SourceProps.AiOverlayTip"));

	/* Monitor-only is listed first and is the default: the program output is
	   what the recording and the stream encode, and a box burned into it is
	   not undoable afterwards. Choosing Program is therefore the deliberate
	   act, not the accident. The trade is that the standard preview pane
	   blits the program mix rather than rendering the source
	   (UI/window-basic-main.cpp RenderMain -> obs_render_main_texture_src_color_only
	   whenever Studio Mode is off), so monitor-only boxes cannot appear in
	   that pane however the plugin behaves. They are visible where the source
	   *is* rendered in its own right: this Properties dialog's own preview,
	   the Filters dialog, a Windowed Projector (the dock's "Monitor window"
	   button), and the Studio Mode preview pane. */
	obs_property_t *mode = obs_properties_add_list(
	    props, PROP_AI_OVERLAY_MODE,
	    obs_module_text("ZCameraPlugin.SourceProps.AiOverlayWhere"),
	    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode,
				  obs_module_text("ZCameraPlugin.AiOverlayWhere.Monitor"),
				  ZC_AI_OVERLAY_MONITOR);
	obs_property_list_add_int(mode,
				  obs_module_text("ZCameraPlugin.AiOverlayWhere.Program"),
				  ZC_AI_OVERLAY_PROGRAM);
	obs_property_set_long_description(
	    obs_properties_get(props, PROP_AI_OVERLAY_MODE),
	    obs_module_text("ZCameraPlugin.SourceProps.AiOverlayWhereTip"));
#endif
#endif
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
	obs_data_set_default_int(settings, PROP_SYNC_MODE, SYNC_INTERNAL);
	obs_data_set_default_bool(settings, PROP_WAIT_I_FRAME, false);
	obs_data_set_default_bool(settings, PROP_LED_TALLY, false);
	obs_data_set_default_bool(settings, PROP_LOW_NOISE, false);
#ifdef ENABLE_CAMERA_CONTROL
	/* Off by default: it opens a second connection to the camera per source. */
	obs_data_set_default_bool(settings, PROP_AI_OVERLAY, false);
	/* Monitor-only by default. A box drawn into the program is also a box in
	   the recording and the stream, and that mistake is not undoable after the
	   fact -- the boxes exist to help frame a shot, not to be published -- so
	   the safe side is the default and the other entry is the deliberate
	   choice. Watch the boxes in the Properties dialog's own preview, in a
	   Windowed Projector, or in the Studio Mode preview pane; the dock's Auto
	   framing tab opens a projector window for this camera in one click. */
	obs_data_set_default_int(settings, PROP_AI_OVERLAY_MODE,
				 ZC_AI_OVERLAY_MONITOR);
#endif
}

static void *zc_source_create(obs_data_t *settings, obs_source_t *source)
{
#ifdef ZC_MAC_ZERO_COPY
	/* Zero-copy: the source draws its own texture, so it is not async. */
	ZcSspSource *s = new (std::nothrow) ZcSspSource(source);
#elif defined(__APPLE__)
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

/* The Interact window's plumbing. libobs only calls these when the source is
   shown in the Interaction dialog (UI/window-basic-interaction.cpp is the one
   caller of obs_source_send_mouse_*) and it has already mapped the pointer to
   the source's pixel space, so the coordinates need no scaling of our own. */
static void zc_source_mouse_click(void *data,
				  const struct obs_mouse_event *event,
				  int32_t type, bool mouse_up, uint32_t click_count)
{
	(void)click_count;
	auto *self = static_cast<ZcSspSource *>(data);
	if (!self)
		return;
	/* The button constants are libobs' (MOUSE_LEFT/…); anything else is not
	   a button this overlay acts on, but it is still passed up so the dock
	   can decide. */
	self->onPointerEvent(mouse_up ? zc::OverlayPointerRelease
				      : zc::OverlayPointerPress,
			     (int)type, event ? event->x : 0, event ? event->y : 0);
}

static void zc_source_mouse_move(void *data,
				 const struct obs_mouse_event *event,
				 bool mouse_leave)
{
	auto *self = static_cast<ZcSspSource *>(data);
	if (!self || mouse_leave)
		return;
	self->onPointerEvent(zc::OverlayPointerDrag, -1, event ? event->x : 0,
			     event ? event->y : 0);
}

static uint32_t zc_source_get_width(void *data)
{
	return static_cast<ZcSspSource *>(data)->getWidth();
}

static uint32_t zc_source_get_height(void *data)
{
	return static_cast<ZcSspSource *>(data)->getHeight();
}

/* What colour space the decoded picture is in.

   A source that does not answer this is taken to be sRGB (obs-source.c,
   obs_source_get_color_space), and whenever the renderer's space differs
   libobs converts the picture first - by drawing the whole source into a
   canvas-sized off-screen texture and compositing that (obs-source.c,
   source_render, the "convert_tech" branch). That is what made the AI
   overlay's "where am I being drawn?" test useless: inside video_render()
   every pass looked identical, because the program mix, the Properties
   preview, a Windowed Projector and the Studio Mode panes all arrived as a
   full-canvas off-screen pass, so the box code could not tell the pass the
   recording encodes apart from a monitor pane - and it either drew into the
   recording or drew nowhere.

   Saying the picture is already in the target's space removes that
   conversion, and with it the extra pass: every caller then reaches
   video_render() as itself, a display with no render target or the canvas
   mix, which is exactly what the overlay needs to see.

   This is what libobs does for its own async video sources (obs-source.c,
   the OBS_SOURCE_ASYNC branch: it hands back a preferred space when none
   matches the source's native one). The picture is BT.709 camera video, so
   claiming the canvas's 709 or a preview's sRGB is a statement about how to
   interpret it, not a conversion being skipped. */
static enum gs_color_space zc_source_get_color_space(
	void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	(void)data;
	if (count > 0 && preferred_spaces)
		return preferred_spaces[0];
	return GS_CS_709_EXTENDED;
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
#ifdef ZC_MAC_ZERO_COPY
	/* Zero-copy on macOS: decode into a BGRA IOSurface and draw it here, so
	   the source owns its rendering like it does on Windows. */
	info.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW |
		OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_INTERACTION;
#elif defined(__APPLE__)
	/* No zero-copy build: the OpenGL backend cannot import the camera's 420v
	   IOSurface, so decode in software and let OBS upload/convert the frame
	   asynchronously. This is also the path that keeps 10-bit / HDR. */
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_INTERACTION;
#else
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_INTERACTION;
#endif
	info.get_name = zc::zc_source_getname;
	info.get_properties = zc::zc_source_getproperties;
	info.get_defaults = zc::zc_source_getdefaults;
	info.create = zc::zc_source_create;
	info.destroy = zc::zc_source_destroy;
	info.update = zc::zc_source_update;
	info.activate = zc::zc_source_activate;
	info.deactivate = zc::zc_source_deactivate;
	/* video_tick drives the stall watchdog on every platform, so a hidden or
	   unrendered source still notices a dead stream; on a non-zero-copy macOS
	   build (ASYNC_VIDEO, no video_render) it is the only path, elsewhere it
	   supplements video_render (watchdog() is idempotent). */
	info.video_tick = zc::zc_source_video_tick;
#if !defined(__APPLE__) || defined(ZC_MAC_ZERO_COPY)
	info.video_render = zc::zc_source_video_render;
#endif
	info.get_width = zc::zc_source_get_width;
	info.get_height = zc::zc_source_get_height;
	/* Keep libobs from wrapping every pass in a colour-space conversion, which
	   is what stops the AI overlay from telling a monitor pane from the pass
	   the recording encodes - see zc_source_get_color_space(). */
	info.video_get_color_space = zc::zc_source_get_color_space;
	/* Interact-window pointer input. The dock arms a mode and answers the
	   events with camera writes; with no mode armed these do nothing. */
	info.mouse_click = zc::zc_source_mouse_click;
	info.mouse_move = zc::zc_source_mouse_move;
	info.enum_active_sources = zc::zc_source_enum_active_sources;
	return info;
}
