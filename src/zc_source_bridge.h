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

/* One of the camera's two framing frames: the tracking-box ("range") region and
   the lock-angle ("fix angle") region.

   Both are stored by the camera as the pan/tilt the box was saved at plus the
   focal ratio then in use — never as a rectangle in the picture (bugs 53/243/
   244). `roiLeft/Top/Width/Height` only narrows that pose to the sub-rectangle
   the operator drew inside it, exactly as the page's `applyRoiToWorldBox` does;
   an un-cropped box is 0,0,1,1. A rectangle in the picture only exists once the
   pose is looked at from wherever the head is now, so the box pose and the view
   pose both travel to the source, which projects them at draw time — it is the
   only side that knows the picture's aspect ratio. */
struct WorldBoxPose {
	bool valid = false;      /* draw it at all (switched on, both poses known) */
	float pan = 0.0f, tilt = 0.0f;
	float hfov = 0.0f;       /* horizontal field of view at that pose, degrees */
	float roiLeft = 0.0f, roiTop = 0.0f;
	float roiWidth = 1.0f, roiHeight = 1.0f;
};

/* Which frame a setOverlayWorldBox() call is about. They differ only in how they
   are painted: the page dashes the range (the operator may own it) and draws the
   lock-angle boundary solid. */
enum WorldBoxKind {
	WorldBoxRange = 0,
	WorldBoxLockAngle = 1,
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
	/* A framing frame, projected from its saved pose onto today's picture.
	   `view` is where the head is now (its roi fields are ignored). Two calls,
	   one per frame, each carrying the same view pose. */
	virtual void setOverlayWorldBox(int kind, const WorldBoxPose &box,
					const WorldBoxPose &view) = 0;
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
