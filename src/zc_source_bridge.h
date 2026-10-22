/*
 * obs-zcamera — source <-> control-dock bridge for on-video interaction.
 *
 * The camera source (ZcSspSource, zc_source.cpp) renders the video and owns
 * the Interact window's drawing, but it deliberately holds no ZcCameraClient:
 * a source that issued camera commands on construction would move the camera
 * just by being added to a scene. The dock (zc_control_dock.cpp) is the one
 * place that owns a client and a connection.
 *
 * So the two meet here. The source forwards pointer events from its Interact
 * window through the single OverlayPointerSink the dock registers; the dock
 * answers with the camera writes only it can make, and pushes back the state
 * the source should paint (the detection-area rectangle and the click
 * crosshair) through the OverlayTarget registered under the camera address.
 *
 * All callbacks run on the OBS UI thread (the Interact window is a UI
 * widget), which is also the thread the dock and the source's settings run
 * on, so no extra locking is needed between the two sides.
 */
#pragma once

#include <QString>

namespace zc {

/* What a click on the source's Interact window means. The camera's own web
   page has the same two: its toolbar cursor icon moves the head to the
   clicked point, and its framing page drags the detection rectangle. */
enum class OverlayInteraction {
	None = 0,  /* read-only: the overlay paints, the pointer does nothing */
	PtzClick,  /* click a point -> turn the head to it                   */
	MaskEdit,  /* drag -> define the detection-area rectangle            */
};

/* Pointer phases handed to onOverlayPointer(). */
enum OverlayPointerPhase {
	OverlayPointerPress = 0,
	OverlayPointerDrag = 1,
	OverlayPointerRelease = 2,
};

/* What a press on the detection-area rectangle starts.

   The camera's own page lets the operator work on the rectangle it already
   has: grab a corner to resize it, grab the inside to move it, press outside
   to draw a new one. The plugin used to start a new rectangle on every press,
   so the first click of an edit destroyed the one being edited (bug 212). */
enum class MaskGesture {
	None = 0,
	New,          /* press on empty picture: draw a fresh rectangle        */
	Move,         /* press inside: carry the whole rectangle along        */
	ResizeLeftTop,
	ResizeRightTop,
	ResizeLeftBottom,
	ResizeRightBottom,
};

/* How far from a corner (normalized units) a press still counts as "on that
   corner". Both sides of the bridge use this one number, so the rectangle the
   source draws and the one the dock writes cannot disagree. */
constexpr float kMaskGrabTolerance = 0.025f;

/* What a press at (nx, ny) starts, given the rectangle currently on the
   picture. An empty or switched-off rectangle always starts a new one. */
MaskGesture maskGestureAt(float left, float top, float right, float bottom,
			  bool on, float nx, float ny);

/* The rectangle a drag produces: from the press state (`base*`, the corners
   before the press; `grabX`/`grabY`, where the pointer went down) to the
   pointer at (nx, ny). Corners stay ordered and inside the picture, and a
   resize never crosses over its own fixed corner. For MaskGesture::New the
   grab point is the anchor, so the rectangle is grown from it. */
void maskDragRect(MaskGesture gesture, float baseL, float baseT, float baseR,
		  float baseB, float grabX, float grabY, float nx, float ny,
		  float *outL, float *outT, float *outR, float *outB);

/* Implemented by the dock. One sink at a time: the dock drives one camera. */
class OverlayPointerSink {
public:
	virtual ~OverlayPointerSink() = default;
	/* `nx`, `ny` are normalized to the decoded picture (0..1, y down).
	   `button` is MOUSE_LEFT / MOUSE_MIDDLE / MOUSE_RIGHT from libobs. */
	virtual void onOverlayPointer(int phase, int button, float nx, float ny) = 0;
};

/* Implemented by ZcSspSource; looked up by the camera address. */
class OverlayTarget {
public:
	virtual ~OverlayTarget() = default;
	virtual void setOverlayInteraction(OverlayInteraction mode) = 0;
	/* The camera's detection-area rectangle, normalized (left<right,
	   top<bottom). Drawn while an interaction mode is active. */
	virtual void setOverlayMask(bool enabled, float left, float top,
				    float right, float bottom) = 0;
	/* Where the user last clicked, for the crosshair. */
	virtual void setOverlayMarker(bool on, float nx, float ny) = 0;
	/* The camera's tracking-box ("range") frame, normalized left/top plus
	   size. Drawn in every non-program pass while enabled — the region line
	   the web preview shows (bug list 17). */
	virtual void setOverlayTrackBox(bool enabled, float left, float top,
					float width, float height) = 0;
	/* The lock-angle ("fix angle") region (bug 53). The camera keeps it as the
	   pan/tilt it was saved at plus the focal ratio then in use, NOT as a
	   rectangle in the picture: it only becomes a rectangle once it is looked
	   at from wherever the head is now. So the dock sends the two poses and
	   their horizontal fields of view, and the source projects them onto the
	   picture it is drawing into (which is what the camera's own page does
	   with its WebGL canvas — ptzToWorldBox + viewProjectionFromPtz). */
	virtual void setOverlayAngleBox(bool enabled, float boxPan, float boxTilt,
					float boxHfov, float viewPan,
					float viewTilt, float viewHfov) = 0;
};

/* Register / replace the dock's sink. Pass nullptr to detach. */
void overlaySetPointerSink(OverlayPointerSink *sink);

/* Called by the source from its mouse callbacks. */
void overlayDispatchPointer(int phase, int button, float nx, float ny);

/* The source registers itself under its camera address so the dock can find
   the live object (obs_enum_sources only yields names). */
void overlayRegisterTarget(const QString &ip, OverlayTarget *target);
void overlayUnregisterTarget(const QString &ip, OverlayTarget *target);
OverlayTarget *overlayFindTarget(const QString &ip);

} /* namespace zc */
