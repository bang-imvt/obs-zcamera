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
