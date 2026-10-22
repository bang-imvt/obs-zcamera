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

/* Camera control dock. Three-panel layout mirroring the ZCamGuiOpen client:
   camera rail (left), live preview + HUD (center), settings/PTZ sidebar
   (right), with a debug panel toggled from the status bar. */

#pragma once

#include <QWidget>
#include <QDockWidget>
#include <QPointer>
#include <QListWidget>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QPainterPath>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QHash>
#include <QPixmap>
#include <QVector>

/* ZcCredentials, held per host for the session (bug 4). */
#include "zc_http_transport.h"
/* The overlay interaction modes and the sink the source calls back into. */
#include "zc_source_bridge.h"

class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QComboBox;
class QSlider;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QFrame;
class QBoxLayout;
class QScrollArea;
class QTabWidget;
class QPaintEvent;
class QMouseEvent;
class QTimer;
class QDialog;

/* libobs callback types, declared exactly as <callback/signal.h> and
   <callback/calldata.h> do (obs.h is included after this header). */
struct signal_handler;
typedef struct signal_handler signal_handler_t;
struct calldata;
typedef struct calldata calldata_t;

namespace zc {

class ZcCameraClient;
class ZcDebugPanel;

/* One rail row. `offline` marks a camera that is known only from OBS as a
   source: discovery is not advertising it, so it cannot be controlled until it
   answers (spec §1, L11). */
struct ZcRailCamera {
	QString name;
	QString host;
	bool offline = false;
};

/* Camera rail: one row per camera, drawn as two lines (name above address) with
   the row's own add-to-scene chip. A click selects the camera for control only;
   adding it to OBS is the chip's job (spec §1). */
class ZcCameraRail : public QWidget {
	Q_OBJECT
public:
	explicit ZcCameraRail(QWidget *parent = nullptr);
	QListWidget *list() const { return list_; }

	/* Rebuild the list from the given cameras, discovered ones first. No-op
	   when the same rows would be produced, so the selection is kept and the
	   dock does not flicker on every scan. */
	void setCameras(const QVector<ZcRailCamera> &cameras);

	/* Mark one row offline (or clear the mark) in place, without rebuilding the
	   list — the row was just clicked and must keep its selection. */
	void setHostOffline(const QString &host, bool offline);

	/* Put the highlight on one camera without emitting cameraClicked(). The
	   camera an OBS source represents is picked from outside the rail (the
	   source's Properties dialog), and the row used to stay on whatever was
	   selected in the previous session — the control page then read as "the
	   other camera" (bug 149). A host that has no row yet is remembered and
	   selected by the next setCameras(): a source is listed only after the
	   following scan. */
	void selectHost(const QString &host);

	/* The subset of the listed hosts that already exist in OBS as a source.
	   Rows for those hosts get the "added" badge and their chip turns into a
	   gold check instead of "＋" (spec §1, L4/L5). */
	void setAddedHosts(const QSet<QString> &added);

	/* Wait, scanning: replace the (empty) list with a busy page. Set while
	   discovery has not produced a single row yet, so an empty rail reads as
	   work in progress rather than as a scan that found nothing. */
	void setBusy(bool busy);

	/* Spin the rescan button while a manual rescan is in flight, and stop it
	   when the answers have landed. A second click during that window would
	   only queue a duplicate query, so the button is disabled for the
	   duration as well. */
	void setRefreshing(bool refreshing);

signals:
	void cameraClicked(const QString &host);
	/* A row's chip was clicked on a camera that is not yet a source: add that
	   camera to the current scene. The chip acts on its own row, so no selection
	   is needed first (spec §1, L6). */
	void addRequested(const QString &host);
	/* A row's chip was clicked on a camera that IS already a source: remove the
	   source from OBS (spec §1, L5). */
	void removeRequested(const QString &host);
	/* The rail header's rescan button was pressed. Discovery runs on its own
	   schedule (a 2.5 s poll over a 1 s mDNS query), so this asks for an
	   immediate one: re-read the records now and again once the replies to the
	   forced query have landed. */
	void refreshRequested();

private:
	/* Compose the fields, glyph and tooltip of one row from its stored fields.
	   The label itself is painted by ZcRailDelegate. */
	void applyRow(QListWidgetItem *item);
	/* Show exactly one of the three pages: the list once it has rows, the
	   busy spin while scanning has yet to name one, and the "no camera" line
	   once scanning has given up. */
	void updateEmptyState();

	QListWidget *list_ = nullptr;
	/* The rail header's rescan button (see refreshRequested()). */
	QToolButton *refreshBtn_ = nullptr;
	/* Turns refreshBtn_'s glyph while a rescan is in flight, so the press has a
	   visible effect even though the list it rebuilds usually does not change
	   (see setRefreshing). */
	QTimer *spinTimer_ = nullptr;
	int spinAngle_ = 0;
	QSet<QString> added_;
	/* The host selectHost() was asked for but the list does not hold yet. */
	QString pendingSelectHost_;
	/* Starts busy: the dock is built before the first scan can run, and a rail
	   that reads "no camera" during that first tick would be wrong. */
	bool busy_ = true;
	bool hasRows_ = false;
	QWidget *busyPage_ = nullptr;
	QWidget *emptyPage_ = nullptr;
};

/* Transparent HUD over the preview. */
class ZcHud : public QWidget {
	Q_OBJECT
public:
	explicit ZcHud(QWidget *parent = nullptr);

	QLabel *recBadge() const { return recBadge_; }
	QLabel *durationLabel() const { return durationLabel_; }
	QLabel *modeLabel() const { return modeLabel_; }
	QList<QLabel *> infoChips() const { return chips_; }

public slots:
	void setRecording(bool rec);
	void setMode(const QString &mode);
	void setTemperature(double celsius);
	void setInfo(const QString &iso, const QString &shutter,
		     const QString &ev, const QString &wb, const QString &iris,
		     const QString &fps, const QString &res,
		     const QString &temp);
	void setDuration(int seconds);
	void setRemaining(int seconds);

private:
	QLabel *recBadge_ = nullptr;
	QLabel *durationLabel_ = nullptr;
	QLabel *modeLabel_ = nullptr;
	QLabel *remainingLabel_ = nullptr;
	QList<QLabel *> chips_;
};

/* Settings sidebar (stream bar + group subtabs + generated rows). Rows are built
   from the camera's own getbatch reply: unsupported and read-only keys are
   hidden, and an edit writes immediately (no per-row Set button). */
class ZcSettingsPanel : public QWidget {
	Q_OBJECT
public:
	explicit ZcSettingsPanel(QWidget *parent = nullptr);

	void setClient(ZcCameraClient *client);
	void selectGroup(const QString &catalog);

	/* Show an in-page login form (setClient has no client because the camera
	   answered 401) or a "switch to http" notice (HTTPS-only camera, no TLS
	   backend). hidden again by hidePrompts() once connected. */
	void showLoginPrompt(const QString &host);
	void showTlsPrompt(const QString &host);
	/* The submitted credentials were rejected: keep the form up and say so
	   (bug list "wrong credentials are not validated or reported"). */
	void showLoginError();
	/* Five rejected logins in a row: the camera locks the account out, so the
	   form says so and stops accepting clicks until a fresh prompt (bug list
	   "after 5 wrong passwords the login dialog vanishes and the main UI
	   loads empty"). */
	void showLoginLocked();
	void hidePrompts();
	/* The camera is in its playback mode (bug 167): /ctrl/mode says so, either
	   from a ModeChanged event or from the poll in the client. Everything on
	   this pane ignores writes in that mode, so say why at the top of the pane
	   and lock the controls instead of letting the operator click into a void
	   — the camera answers {"code":0} to a write it discards. */
	void setPlaybackMode(bool on);

public slots:
	/* Re-read the stream bar and the visible group from the camera. */
	void refreshGroup();

signals:
	/* A write the camera applies by restarting (HTTPS, identity auth) or by
	   moving to another address (network type) was issued: the control
	   connection is about to go away until the camera is back (bug 8). */
	void cameraRestarting();
	/* The operator filled in the in-page login form. */
	void loginRequested(const QString &user, const QString &password);

private slots:
	/* A catalog finished loading; rebuild the rows when it is the visible one. */
	void onGroupRefreshed(const QString &catalog);
	void onGroupSelected(const QString &catalog);

private:
	void populateGroups();
	void buildRows(const QString &catalog);
	/* Write one setting and, when the camera applies it by restarting, tell the
	   dock so it can wait for the camera and reconnect (bug 8). */
	void writeKey(const QString &key, const QString &value);
	/* Show only the row the PTZ "common speed unit" selects: the speed row for
	   unit 0, the time row for unit 1, hiding the other (bug list "switching the
	   common speed unit does not show the matching speed/time row"). */
	void updatePtzCommonRows();

	/* The stream bar: the encoder's own document, always at the top of the tab
	   rather than in the group list, because these are the settings that matter
	   most for a network stream (spec §2). Built once — the controls are updated
	   in place, so a write's reply cannot close a box the operator is using. */
	void buildStreamBar(QBoxLayout *outer);
	void updateStreamBar();
	/* Read the stream document and repaint. The first call also discovers which
	   index the firmware offers; a no-op without a client. */
	void loadStream();
	/* Apply fields to the stream, then repaint from the camera's own reply so
	   the bar never shows a value the camera did not take. */
	void writeStreamParams(const QList<QPair<QString, QString>> &params);

	ZcCameraClient *client_ = nullptr;
	QListWidget *subtabList_ = nullptr;
	QWidget *rowsContainer_ = nullptr;
	/* The label + control widgets of each rendered row, keyed by setting id, so
	   a row can be hidden/shown without rebuilding the pane (used by the PTZ
	   common-speed-unit linkage). Rebuilt on every buildRows(). */
	QHash<QString, QList<QWidget *>> rowWidgets_;
	/* The catalog currently rendered, so a late getbatch reply knows whether
	   it still owns the rows pane. */
	QString catalog_;

	/* In-page login / HTTPS fallback notice (shown until the camera is
	   connected; hidden once it is). */
	QWidget *loginPage_ = nullptr;
	QLabel *promptHint_ = nullptr;
	/* Rejection notice shown under the form after a failed login. */
	QLabel *loginError_ = nullptr;
	/* The username/password fields + login button, wrapped as one unit so the
	   HTTPS notice can hide the whole form at once (labels included). */
	QWidget *loginFields_ = nullptr;
	QLineEdit *loginUser_ = nullptr;
	QLineEdit *loginPass_ = nullptr;
	/* Reveal (show/hide) toggle for the password row (bug list #30). */
	QToolButton *passReveal_ = nullptr;
	QPushButton *loginBtn_ = nullptr;
	QString loginHost_;

	/* Playback-mode banner (bug 167): amber line above the stream bar, shown
	   while the camera reports a playback mode. */
	QLabel *playbackBanner_ = nullptr;
	/* The last state painted into the banner, so a repaint on every poll does
	   not re-layout the pane. */
	bool playbackShown_ = false;

	/* Stream bar controls. */
	QWidget *streamBar_ = nullptr;
	QComboBox *streamCodec_ = nullptr;
	QComboBox *streamRes_ = nullptr;
	QComboBox *streamFps_ = nullptr;
	QSlider *streamBitrate_ = nullptr;
	QLabel *streamBitrateValue_ = nullptr;
	QSlider *streamGop_ = nullptr;
	QLabel *streamGopValue_ = nullptr;
	/* The index, bit width and streaming state, as one muted line. */
	QLabel *streamInfo_ = nullptr;

	/* Stream state: the index the bar describes and its last reply. Both survive
	   a rebuild of the rows. The index is discovered from the camera on the
	   first read and then fixed (spec §2 Stream); it is emptied when the camera
	   changes, so a late reply can never paint another camera's — or another
	   index's — numbers. */
	QString streamIndex_;
	QJsonObject streamDoc_;
};

/* Circular 8-direction PTZ joystick + speed/home/stop. The annular
   ring is painted in paintEvent(); hold-to-move sectors (or drag the
   center knob) drive ptzMove(), release calls ptzStop(). */
class ZcOverlayLink;

class ZcPtzPad : public QWidget {
	Q_OBJECT
public:
	explicit ZcPtzPad(QWidget *parent = nullptr);

	void setClient(ZcCameraClient *client);
	/* The shared interaction link: the pad arms "click to move" through it
	   (see ZcOverlayLink). */
	void setOverlayLink(ZcOverlayLink *link) { link_ = link; }
	/* The click-to-move button in the dial row, so the dock can keep it in
	   step with the framing panel's detection-area toggle: both arm the same
	   slot on the source, so arming one has to clear the other. */
	QToolButton *clickMoveButton() const { return targetBtn_; }

	/* Re-read the current position and the range limit from the camera and
	   repaint. A no-op without a client. */
	void refreshState();
	/* Live position from a ptzfChange notification. */
	void setPosition(double pan, double tilt);

signals:
	void moveRequested(int pan, int tilt);
	void homeRequested();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	/* Start / stop the range-limit poll with visibility (see
	   limitPollTimer_). */
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	enum class JoystickMode { None, Sector, FreeDrag };
	/* The four edges of the travel box, in the order the sliders are stacked
	   (matching the camera's web UI "Range Limit" group). */
	enum Edge { EdgeUp = 0, EdgeRight, EdgeDown, EdgeLeft, EdgeCount };

	/* Recompute joystick geometry from the (fixed-size) joystick area. */
	void updateGeometry();
	/* Sector index (0..7) at widget pos, or -1 if outside the ring. */
	int directionAt(const QPoint &pos) const;
	bool inCenter(const QPoint &pos) const;
	QPainterPath sectorPath(int i) const;
	void moveInDirection(int dirIndex);
	void moveAnalog(const QPointF &offset);
	void stopMoving();

	/* Lay the four range-limit sliders out from the stored limit/unlimit box:
	   with the limit on they sit on the stored boundary, with it off on the
	   camera's full travel. */
	void applyLimitBox(bool enabled);
	/* Send the corner an edge belongs to (up/right -> "upright", down/left ->
	   "downleft"), taking the other axis from the neighbouring slider. */
	void pushLimit(Edge edge);
	/* Refresh one edge's degree read-out. */
	void updateLimitLabel(Edge edge);
	/* Set one edge to the head's current position (the web UI's "Current"). */
	void setEdgeToCurrent(Edge edge);
	/* Put one edge back on the head's full travel (the web UI's "Reset"). */
	void setEdgeToReset(Edge edge);

	/* Lens zoom. The camera's /ctrl/lens?action=zoomin|zoomout&fspeed= is a
	   continuous move that only stops on zoomstop, and this firmware reports no
	   zoom position at all (`z_status` answers {"moving":0}, and the
	   `lens_zoom_pos` key is absent from its lens catalog), so the slider is a
	   spring-return rocker rather than a position control: dragging up zooms
	   in, down zooms out, speed follows the distance from the centre, and
	   releasing recentres it and sends zoomstop. It is vertical and sits beside
	   the joystick with T over W, which is how the camera's own page draws it
	   (its #tab-ctrl-af-zoom-pos column) — the two controls that move the lens
	   belong together, and the speed slider keeps a row of its own because it
	   is not a lens command at all. `pushZoom` sends a command only when the
	   direction or the speed bucket changes, because every request hits the
	   camera's single-threaded httpd and one drag fires hundreds of value
	   changes. */
	void pushZoom(int value);
	void updateZoomLabel(int value);

	ZcCameraClient *client_ = nullptr;
	/* The interaction link, shared with the dock (click-to-move). */
	ZcOverlayLink *link_ = nullptr;
	QWidget *joystickArea_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QPushButton *homeButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	/* Click-to-move target, in the dial row's top-right corner (see
	   ZcOverlayLink). One button replaces the old "Click to move" tick and
	   "Interact…" pair: armed, it also opens the Interact pane, which is the
	   live view the clicks land in, and it closes that pane again on the way
	   off. */
	QToolButton *targetBtn_ = nullptr;
	/* Watches for the Interact pane disappearing behind the button's back (its
	   own X, or OBS closing it with the source), so the tick cannot claim a
	   window that is gone. */
	QTimer *interactTimer_ = nullptr;
	/* The camera's page holds the range-limit box in memory and the firmware
	   pushes nothing when it changes, so a box edited from the web UI (or from
	   another client) used to reach these sliders only after a reconnect — the
	   reported "change it on the web, the plugin does not follow". Re-read while
	   the pad is on screen, and only while the operator is not dragging one of
	   the sliders (a reply landing mid-drag would snap the handle away). */
	QTimer *limitPollTimer_ = nullptr;
	bool limitDragging_ = false;

	/* Lens zoom rocker (see pushZoom). */
	QSlider *zoomSlider_ = nullptr;
	QLabel *zoomValue_ = nullptr;
	bool zoomDragging_ = false;
	/* Last command sent: direction (-1/0/+1) and speed bucket (0..5), so a
	   drag sends one request per bucket instead of one per pixel. */
	int zoomDir_ = 0;
	int zoomBucket_ = 0;

	/* Current Position read-out (live, from ptzfChange / the query reply). */
	QLabel *positionLabel_ = nullptr;
	/* Range Limit group: enable + four edge sliders + their degree read-outs and
	   per-edge "set to current" buttons (bug list: the Range Limit group was not
	   implemented). */
	QCheckBox *limitCheck_ = nullptr;
	QSlider *limitSlider_[EdgeCount] = {};
	QLabel *limitValue_[EdgeCount] = {};
	/* Two buttons per edge, as on the camera's page (its btn_*_current and
	   btn_*_reset): "Current" stores the head's position as that edge,
	   "Reset" puts the edge back on its full travel. They used to be one
	   glyph button, which read as a reset button (bug 98). */
	QPushButton *limitCurrent_[EdgeCount] = {};
	QPushButton *limitReset_[EdgeCount] = {};
	bool limitLoaded_ = false;
	/* Stored limit (current) and unlimit (full travel) box, in whole degrees. */
	int panMin_ = 0, panMax_ = 0, tiltMin_ = 0, tiltMax_ = 0;
	int upanMin_ = 0, upanMax_ = 0, utiltMin_ = 0, utiltMax_ = 0;

	QRectF joystickRect_;
	QPointF joyCenter_;
	qreal joyOuterRadius_ = 96.0;
	qreal joyInnerRadius_ = 44.0;

	JoystickMode mode_ = JoystickMode::None;
	int activeDir_ = -1;
	QPointF dragOffset_;
	/* Centre-knob analog move (see moveAnalog): the last speeds sent, so a
	   drag streams one request per change, not one per mouse move. */
	bool analogMoving_ = false;
	float analogPan_ = 0.0f;
	float analogTilt_ = 0.0f;
};

/* Auto-framing, in its own tab, mirroring the camera's own Auto Framing pages
   control for control, so an operator who knows the web UI can read it without
   relearning it.

   SHOWN IN THE UI: this panel speaks the whole /ctrl/framing API and
   ZcControlDock inserts it as its own tab for a camera whose /info reports auto
   framing. `kFramingPanelVisible` in zc_control_dock.cpp is the single switch
   that shows/hides the tab.

   The panel is split exactly like the camera page:
     - "Live" is its live Auto Framing panel — one status word, the three
       Enable / Start / Restart buttons, the same two columns of tracking
       controls in the same order, the four-arrow framing pad that nudges the
       subject's position in frame, the vertical view-angle (FOV) slider, the
       composition presets, and Refresh.
     - "Settings" is its Auto Framing settings page — the power-on and gesture
       switches, the two delays, the framing-box control and the framing
       profile export.

   Two deliberate departures:
     - Every control writes on change, as the page's do; there is no Apply
       button, so no control can ever be written with a value it is not
       showing.
     - The page draws the live camera frame inside the framing pad. That frame
       belongs to the OBS source, not to this dock, so the pad draws a schematic
       frame and prints the two offsets it is changing.

   Reads come from one /ctrl/framing/all_status snapshot (plus /fov and
   /status); writes are the same /ctrl/framing/<sub> calls the web UI makes. */

class ZcSegmentedControl;
class ZcSliderSpin;
class ZcFramingPad;

class ZcFramingPanel : public QWidget {
	Q_OBJECT
public:
	explicit ZcFramingPanel(QWidget *parent = nullptr);

	void setClient(ZcCameraClient *client);
	/* Told whenever this panel's idea of "auto framing is driving the head"
	   changes. The PTZ tab's trace face hangs off it: while framing frames the
	   shot the camera refuses every trace command with code -2, so those
	   buttons must grey rather than swallow the click. It is the panel that
	   says so because the panel is what receives the snapshots — including the
	   one read at connect time, which is the only way to learn that a camera
	   was already framing before the dock attached (bug lists 55/179). */
	std::function<void(bool)> onFramingOwnsPtz;
	/* The shared interaction link: the detection-area editor arms through it
	   and the camera's rectangle is pushed back through it. */
	void setOverlayLink(ZcOverlayLink *link) { link_ = link; }
	/* The mask rectangle the camera last reported, so the source can draw it
	   even before the operator touches anything. */
	void setMaskFromCamera(bool enabled, float l, float t, float r, float b);
	/* Firmware generation gate (bug list #10/#11/#48/#49): true when the
	   camera's `sw` is at least 1.0.5, which removes the View Angle / Size /
	   Detected Indicator controls and adds the Size slider and Lock-angle
	   interlocks. Re-evaluates row visibility; snapshots re-and it with key
	   presence. */
	void setFirmwareNew(bool atLeast15);
	/* The "edit detection area" tick, for the dock's mode sync. */
	QPushButton *maskEditButton() const { return maskEditBtn_; }
	/* Re-ask the monitor button what is up. The dock does it when a tab is
	   switched (bug 219): the page can be left and come back while the window
	   stays on screen, and the tick has to be right the moment the page is
	   shown instead of up to a poll later. */
	void resyncMonitorButton() { syncMonitorButton(); }
	/* Self-test only (ZC_TEST_FLOW): click the detection-area switch and its
	   editor the way an operator does, so a run can prove the whole path —
	   write, read-back, rectangle, drag — without a mouse. */
	void zcTestOperatorFlow();
	void zcTestOperatorFlowClicks();
	/* Told after the operator flips the framing box (mode or enable), so the
	   dock re-reads its rectangle and repaints the overlay frame: the camera's
	   settings-changed event is for changes made elsewhere and does not fire
	   for our own write, which left the monitor window without a frame (bug
	   53). */
	std::function<void()> onBoxStateChanged;
	/* Whether the camera is recording right now. The "tracking triggers
	   recording" row shows it: the camera starts recording on its own, so the
	   switch alone never told the operator it had done anything (bug 93). */
	void setRecording(bool on);

public slots:
	/* Re-read the framing snapshot and the preset list, then repaint. */
	void refresh();
	/* Paint from a framing snapshot (/ctrl/framing/all_status) or a live
	   FramingStatusChanged payload: only the fields present are applied. */
	void applyStatus(const QJsonObject &doc);

protected:
	/* The lock-angle overlay follows the head, so it is only sampled while
	   this page is on screen (see syncAngleBoxPoll). */
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	/* Read the detection-area rectangle from /ctrl/framing/mask_area and hand
	   it to the source. all_status does not carry mask_left/... on every
	   firmware, and nothing else ever pushed a rectangle — so the overlay had
	   nothing to draw and the monitor window showed the picture with no frame
	   on it at all (bug 28). */
	void refreshMaskRect();
	/* Re-read /ctrl/framing/preset and rebuild the tile list. */
	void refreshPresets();
	void rebuildPresets(const QJsonObject &doc);
	/* Re-read /ctrl/framing/config, which is where the three composition
	   presets' sizes live (its `fov_size` is a structure; the scalar of the
	   same name in all_status is the current size, not the table). Read once
	   per camera. */
	void refreshFovPresets();
	/* The composition size the subject figure should be drawn at, in percent:
	   the >=1.0.5 Size row when the camera has one, else the 1.0.3 body-size
	   stepper, which walks four steps between its own min and max. */
	int subjectSizePct() const;
	/* The framing size the read-out under the pad prints: the number the
	   control that owns it shows. `subjectSizePct()` is the same size on the
	   0..100 scale the figure is drawn with, which is NOT what a 1.0.3 body
	   displays — its Size stepper reads 80..120 % (bug 214). */
	int sizeReadoutPct() const;
	/* Push the two offsets and the subject size into the pad at once. */
	void paintPad();
	/* The handle-gesture explanation the camera's page shows, with its
	   pictures (bug 42). */
	void showGestureHelp();
	/* Re-read /ctrl/framing/fov, which carries the two offsets the framing pad
	   nudges; the page re-reads it after every nudge so its copy cannot drift
	   from the camera's. */
	void refreshFovOffsets();
	/* One probe per optional feature, on the first refresh after a camera is
	   picked: the framing-box control and the profile export both answer
	   {"code":-1} on a firmware without them, and a control that can only fail
	   is better shown disabled with the reason in its tooltip. */
	void probeOptionalFeatures();
	/* Repaint the status word and the Enable/Start/Restart button faces from the
	   live switches. */
	void updateStatusLabel();
	/* Grey the Live page's controls while the camera's detection is off, exactly
	   as the camera's own page does (framing.js `_updateFramingUiStatus` puts
	   `ui-state-disabled` on `.aframe_left_layout`, the whole live control
	   block, because the camera ignores its contents until Enable is on). The
	   Settings page is never greyed — the page leaves it live either way. */
	void updateGating();
	/* Show/hide the rows the firmware generation decides (bug list #10/#11/
	   #14): View Angle + Size + Detected Indicator go, and the Settings page's
	   unsupported rows hide, on a >=1.0.5 camera. */
	void applyFirmwareGating();
	/* Move one /ctrl/framing/fov offset by one step and tell the camera. Only
	   ever called from the framing pad; the read-out follows the write. */
	void nudgeOffset(bool placement, int delta);
	/* The second half of the profile export, run from a queued zero-timer
	   (never inside the HTTP completion callback — see the export button's
	   comment): opens the save dialog and writes pendingExportProfile_. */
	void finishProfileExport();
	/* Open a projector window on this camera's own source. That window is one
	   of the few views that renders the source by itself, so it is where a
	   monitor-only detection overlay can be watched without any of it reaching
	   the recording — the standard preview pane blits the program mix instead
	   and therefore cannot show it. Falls back to a window on the current
	   scene when this camera has no source in OBS yet.

	   There is exactly one such window: OBS's projector API unconditionally
	   constructs a new one, so a second click re-raises the window already
	   open (see monitorWnd_) instead of stacking another. The window is also
	   put above the OBS main window, because it is useless if working in the
	   dock hides the very boxes it exists to show. */
	void openMonitorWindow();
	/* Close that window again (the status row's button is a toggle). */
	void closeMonitorWindow();
	/* The window if one is up for this camera, else null. Also refreshes
	   monitorWnd_/monitorWndName_. */
	QWidget *monitorWindow();
	/* Title suffix for this camera's monitor window; `isSource` reports
	   whether it is the camera's own source rather than the current scene.
	   Empty name means there is nothing to show. */
	QString monitorWindowName(bool &isSource);
	/* Point the status row's button at the truth: checked iff a window is up.
	   Runs on a 1 s poll because the window can be closed from its own title
	   bar and opened from OBS's View menu, neither of which passes through
	   the button. */
	void syncMonitorButton();

	/* The camera's page is three live buttons, not a row of checkboxes:
	   Enable/Disable toggles `detect`, Start/Stop toggles `trace`, and Restart
	   re-runs detection. They act immediately, as on the page. */
	QPushButton *enableBtn_ = nullptr;
	QPushButton *startBtn_ = nullptr;
	QPushButton *restartBtn_ = nullptr;
	/* Opens the projector window described on openMonitorWindow(). Checked
	   means that window is up; a second press closes it. */
	QPushButton *monitorBtn_ = nullptr;
	/* Keeps monitorBtn_ in step with a window opened or closed outside the
	   button. */
	QTimer *monitorPoll_ = nullptr;
	/* The monitor window this panel opened, and the source/scene it shows.
	   OBS has no "is this source already projected?" query, so the handle is
	   kept here and re-raised rather than re-opened; it clears itself when
	   the operator closes the window (OBS's projector windows are built with
	   WA_DeleteOnClose). The name guards a camera switch, which changes the
	   source and so must not reuse the previous camera's window. */
	QPointer<QWidget> monitorWnd_;
	QString monitorWndName_;
	/* What the camera last reported, so the button faces can follow it. */
	bool detectOn_ = false;
	bool traceOn_ = false;
	/* Set while applyStatus() paints the controls from a snapshot. Every change
	   handler returns early on it, so a repaint can never look like an operator
	   edit and write the camera back. This replaces the old Apply button — and
	   with it the chance of applying index-0 defaults over the camera's stored
	   scene / lost action / target count. */
	bool painting_ = false;
	/* Whether this camera exists in OBS as a source, which is the only thing the
	   Detected Indicator switch can act on: it drives the source's AI-box master
	   switch (zc_source.cpp's zc_ai_overlay), so with no source the switch has
	   nothing to write and is shown dead. Set in applyStatus(). */
	bool indicatorSourcePresent_ = false;

	/* The page's Off/On segments, in page order. */
	ZcSegmentedControl *autoTargetSeg_ = nullptr;     /* Live: Auto Target */
	ZcSegmentedControl *zoomLockSeg_ = nullptr;       /* Settings: Lock Zoom */
	ZcSegmentedControl *tiltLockSeg_ = nullptr;       /* Live: Lock Tilt */
	ZcSegmentedControl *maskSeg_ = nullptr;           /* Live: Mask */
	/* "Edit detection area": arms the drag-to-define rectangle on the
	   source's own Interact window (see ZcOverlayLink). */
	QPushButton *maskEditBtn_ = nullptr;
	/* "Register faces…": opens the camera's face database editor
	   (ZcFaceRegDialog). One dialog per camera, kept so a second click
	   raises it instead of opening a duplicate. */
	QPushButton *faceRegBtn_ = nullptr;
	QDialog *faceRegDlg_ = nullptr;
	/* The mask rectangle as last read from /mask_area, for the source. */
	bool maskOn_ = false;
	float maskL_ = 0.0f, maskT_ = 0.0f, maskR_ = 1.0f, maskB_ = 1.0f;
	ZcSegmentedControl *indicatorSeg_ = nullptr;      /* Live: Detected Indicator */
	ZcSegmentedControl *faceTrackSeg_ = nullptr;      /* Live: Face Tracking */
	ZcSegmentedControl *autoStartSeg_ = nullptr;      /* Settings: Start On Power Up */
	ZcSegmentedControl *tallySeg_ = nullptr;          /* Settings: Green Tally */
	ZcSegmentedControl *trackingRecordSeg_ = nullptr; /* Settings: Recording Triggered By Tracking */
	ZcSegmentedControl *wavingSeg_ = nullptr;         /* Settings: Follow Me */
	ZcSegmentedControl *gestureFovSeg_ = nullptr;     /* Settings: View Angle Adjustment */
	/* Settings: the framing-box control's own two segments — which box, and
	   whether it is on. A firmware that answers /ctrl/framing/fix_angle_box with
	   {"code":-1} has neither. */
	ZcSegmentedControl *boxModeSeg_ = nullptr;
	ZcSegmentedControl *boxEnableSeg_ = nullptr;
	/* The two boxes keep one enable flag each on the camera: all_status
	   reports fix_angle_box.enable and tracking_box.enable separately, so the
	   status switch has to display the flag of the mode that is selected
	   rather than one shared state (bug lists 23/24/25/26/27). */
	bool fixAngleBoxOn_ = false;
	bool trackingBoxOn_ = false;
	/* Lock-angle region as a frame on the picture (bug 53). The camera stores
	   a POSE for it, not a rectangle, so both poses and their horizontal
	   fields of view are collected here and handed to the source, which does
	   the projection (it is the only side that knows the picture's aspect).
	   `angleBoxZoom_` is what `angleBoxHfov_` was read for, so a zoom the box
	   stores differently is fetched once and not every tick. */
	void updateAngleBoxFromSnapshot(const QJsonObject &doc);
	void pushAngleBox();
	void fetchAngleBoxHfov(double zoomFactor);
	void syncAngleBoxPoll();
	QTimer *angleBoxPoll_ = nullptr;
	int anglePollTick_ = 0;
	float angleBoxPan_ = 0.0f, angleBoxTilt_ = 0.0f;
	float angleBoxZoom_ = 0.0f, angleBoxHfov_ = 0.0f;
	float angleViewPan_ = 0.0f, angleViewTilt_ = 0.0f;
	float angleViewHfov_ = 0.0f;
	/* Set while a lens?action=hfov reply is in flight, so a slow camera does
	   not queue one request per poll tick. */
	bool angleHfovPending_ = false;
	/* Lock Zoom as the camera last reported it. Kept here rather than read off
	   zoomLockSeg_ because the interlock below forces that segment to Off when
	   the fix-angle box takes the head over, and a gating rule that reads the
	   widget it just wrote then latches: the Size row and the view-angle
	   controls came back to life while the camera still had the zoom locked
	   (bug lists 50/182). */
	bool zoomLockOn_ = false;
	/* Auto Target as the camera last reported it. Same reason as zoomLockOn_:
	   the detection-area button greys while Auto Target is off, and reading
	   that off the segment meant a value changed on the camera's own page was
	   never seen (bug 88). `known` separates "the camera has said" from "it
	   never carries the key", which falls back to the segment. */
	bool autoTargetOn_ = false;
	bool autoTargetKnown_ = false;

	/* The page's dropdowns. */
	QComboBox *viewAngleCombo_ = nullptr; /* Live: View Angle (fov_type) */
	/* The field containers of the rows the firmware decides on (bug list
	   #10/#11/#14): hidden, not dead, per the tester's removal ask. */
	QWidget *viewAngleField_ = nullptr;
	QWidget *bodySizeField_ = nullptr;
	/* The framing pad's whole row: greyed with the view angle while
	   detection is off (bug list 22). */
	QWidget *padField_ = nullptr;
	/* The Composition Presets button-state updater: selection changes and
	   table rebuilds both run it (bug lists 9/44). */
	std::function<void()> presetButtons_;
	QWidget *indicatorField_ = nullptr;
	QWidget *zoomLockRow_ = nullptr;
	QWidget *sensitivityRow_ = nullptr;
	QWidget *accelerationRow_ = nullptr;
	/* Their group box, hidden when the firmware has neither row (bug 186). */
	QWidget *speedGroup_ = nullptr;
	QWidget *trackingRecordRow_ = nullptr;
	/* The "(recording)" word that sits to the right of that row's switch
	   (bug 221): the caption itself stays constant, so a camera that starts
	   recording cannot re-wrap a fixed-width column and shove the buttons
	   down the group. */
	QLabel *trackingRecBadge_ = nullptr;
	/* The >=1.0.5 firmware's "Size" row (fov_size 0..100), shown only when
	   all_status carries the key (bug list #10). */
	QWidget *fovSizeField_ = nullptr;
	ZcSliderSpin *fovSizeSp_ = nullptr;
	/* Whether the selected camera reported a `fov_size` at all, i.e. whether
	   the >=1.0.5 Size row is the one in use. A flag rather than
	   fovSizeField_->isVisible(), which is false whenever the dock is not the
	   visible tab — the subject figure would then read the 1.0.3 stepper. */
	bool hasFovSize_ = false;
	QPushButton *fovFullBodyBtn_ = nullptr;
	QPushButton *fovWaistBtn_ = nullptr;
	QPushButton *fovCloseupBtn_ = nullptr;
	/* Repaint the three size presets so exactly the one matching the current
	   Size carries the tick (bug lists 16/17/20). */
	std::function<void()> fovPresetButtons_;
	int fovPresetFullBody_ = 100, fovPresetWaist_ = 70,
	    fovPresetCloseup_ = 40;
	/* Firmware generation: >= 1.0.5 (see setFirmwareNew). */
	bool fw15_ = false;
	/* The camera's recording flag, for the tracking-record row (bug 93). */
	bool recording_ = false;
	/* The detection-area rectangle has been read once for this camera. */
	bool maskRectRead_ = false;
	/* When this panel last wrote the detection-area switch. The 300 ms
	   snapshot that is already in flight still carries the old value, and
	   painting it back over the operator's click flipped the switch to Off
	   by itself — which greyed out "Edit detection area" and dropped the
	   editor a moment after it opened. The switch is the operator's until
	   the camera has had time to answer. */
	QElapsedTimer maskWriteClock_;
	bool maskWriteRecent() const
	{
		return maskWriteClock_.isValid() &&
		       maskWriteClock_.elapsed() < 1500;
	}
	/* View Angle == Keep (fov_type == 3): the camera keeps its current
	   framing, so the Size stepper has nothing to size — the page greys it
	   (bug list #25). */
	bool fovKeep_ = false;
	QComboBox *sceneCombo_ = nullptr;
	QComboBox *startPosCombo_ = nullptr;
	QComboBox *targetNumCombo_ = nullptr;
	QComboBox *lostActionCombo_ = nullptr;

	/* The page's slider+spinner pairs. The page commits on release and so do
	   these: the camera's httpd is single-threaded, so one write per drag tick
	   would only queue behind itself. */
	ZcSliderSpin *bodySizeSp_ = nullptr; /* Live: Size (fov body_size) */
	ZcSliderSpin *sensitivitySp_ = nullptr;
	ZcSliderSpin *accelerationSp_ = nullptr;
	ZcSliderSpin *lostTimeoutSp_ = nullptr;
	ZcSliderSpin *waitTimeSp_ = nullptr;
	ZcSliderSpin *detectRestartSp_ = nullptr;

	/* The framing pad that moves the subject in frame. */
	ZcFramingPad *pad_ = nullptr;
	/* The numbers under the pad. The page always shows what its nudge arrows
	   did to Face Height / Size; this panel had dropped them, so moving the
	   subject changed nothing on screen (bug 185). */
	QLabel *padReadout_ = nullptr;

	/* Everything the camera's page greys out while detection is off. */
	QList<QWidget *> gatedGroups_;
	/* The framing-box control and the profile export, which this firmware does
	   not have (/ctrl/framing/{fix_angle_box,tracking_box,export} all answer
	   {"code":-1}). The page leaves them on screen but dead; so does this, with
	   the reason in the tooltip, rather than accepting a click that can only
	   fail. */
	QList<QWidget *> optionalRows_;
	QWidget *boxGroup_ = nullptr;
	QWidget *profileGroup_ = nullptr;
	/* The downloaded framing profile waiting for finishProfileExport()'s
	   save dialog (empty + non-empty error text = show the error). */
	QByteArray pendingExportProfile_;
	/* The name the camera gave that file (P2-R1-18x.fprf), used as the save
	   dialog's default so an export is never named .json again — the profile
	   is not JSON and the camera's own upload input only takes .fprf. */
	QString pendingExportName_;
	QString pendingExportError_;
	/* Runs from the last framing-pad nudge. Every write fires a camera
	   notification whose all_status/fov read-back can still carry the value
	   from BEFORE that nudge (the reads queue behind it on the camera's
	   single-threaded httpd), so repainting the two offsets from a read
	   while the operator holds an arrow bounces the numbers back and forth
	   (bug list "held arrow values jump"). While this guard is fresh — a
	   nudge within the last ~0.8 s — the read-backs leave the two offsets
	   alone; the pad's own optimistic value is what the operator sent. */
	QElapsedTimer padNudgeGuard_;
	/* Set once the optional-feature probe has run for the current camera, so a
	   refresh does not re-ask on every tick. */
	bool optionalProbed_ = false;
	/* Same, for /ctrl/framing/config: the preset sizes do not change while the
	   same camera is selected. */
	bool fovConfigProbed_ = false;

	/* Composition presets, at the foot of the live page rather than inside its
	   grid: the block is a list with a row of buttons of its own, and set
	   among the fields it pushed the rest of the grid out of the page's order.
	   It is tracked separately from gatedGroups_ because it is also
	   conditional on the firmware — /ctrl/framing/preset answers {"code":-1}
	   on a camera with no preset table at all, and the group is then hidden
	   outright rather than shown dead. Starts hidden, because nothing is known
	   about a camera until its first answer. */
	QWidget *presetGroup_ = nullptr;
	bool presetSupported_ = false;
	QListWidget *presetList_ = nullptr;
	QPushButton *presetAddBtn_ = nullptr;
	QLabel *presetHint_ = nullptr;
	/* index -> name, as the camera last reported them. */
	QStringList presetNames_;
	int presetCount_ = 0;
	int presetMaxCount_ = 0;
	int presetMaxNameBytes_ = 0;

	QLabel *statusLabel_ = nullptr;
	/* Index into the camera's AI_STATUS_LABELS, i.e. the `status` field of
	   /ctrl/framing/status and the `value` of FramingStatusChanged. */
	int statusIndex_ = 0;
	/* The two /ctrl/framing/fov offsets, as last read or written. */
	int placement_ = 0;
	int faceHeight_ = 0;
	/* Set by the first full snapshot. Controls stay disabled until then, so a
	   control that has never been filled from the camera cannot write its
	   placeholder value back. */
	bool haveSnapshot_ = false;

	ZcCameraClient *client_ = nullptr;
	/* The interaction link, shared with the dock (detection-area editing).
	   owning_ is dropped while the panel paints a pushed snapshot, so the
	   push does not arm a mode by itself. */
	ZcOverlayLink *link_ = nullptr;
};

/* PTZ slot list, covering both slot families the camera exposes: presets
   (/ctrl/preset) and traces, a.k.a. trajectories (/ctrl/ptrace). 10 slots per
   page over 10 pages, plus the per-family action row. Mirrors the camera's own
   web UI preset/trace page (spec §3). */
class ZcPresetPanel : public QWidget {
	Q_OBJECT
public:
	explicit ZcPresetPanel(QWidget *parent = nullptr);

	void setClient(ZcCameraClient *client);

public slots:
	/* Drop the cache and re-read the visible page (a new camera connected). */
	void refresh();
	/* Switch the list between presets and traces. */
	void setTraceMode(bool trace);
	/* The camera's auto framing owns the PTZ, so trace actions are refused
	   (code -2); the preset family keeps working (bug list 55). */
	void setFramingOwned(bool owned);

private:
	/* One slot: thumbnail + name field, click to select. */
	struct Slot {
		QFrame *frame = nullptr;
		QLabel *thumb = nullptr;
		QLineEdit *name = nullptr;
	};

	void buildUi();
	void showPage(int page);
	void loadSlot(int index);
	void applySlotInfo(int index, const QJsonObject &info);
	void onSlotClicked(int index);
	void updateSelection();
	void updateActions();
	void updateSpeedControls();
	/* Read `ptz_speed_mode` once per camera. The PTZ settings catalog — the
	   only place that key is fetched — is read when the operator opens that
	   group, so without this the preset panel had no value to gate on and left
	   the per-preset unit/speed editable under the "Common" mode (bug 74). */
	void askSpeedMode();
	void beginRename();
	void commitRename(int index, const QString &text);
	/* Re-read the /ctrl/ptrace transport state, then re-enable the buttons. */
	void refreshTraceState();
	void onTraceState(int stCode);
	/* A trace action's reply arrived with the camera's `code`: 0 is quiet,
	   anything else shows why nothing happened in the hint line and re-reads
	   the transport state (bug list #137 — a rejected re-record/playback used
	   to fall back to the idle face as if it had succeeded). */
	void reportTraceAction(int code);
	/* Whether the selected slot holds a stored entry (a name). */
	bool selectedIsNamed() const;

	/* The 1-based, zero-padded label an empty slot shows ("Preset 001" /
	   "Trace 001"). */
	QString defaultName(int index) const;

	ZcCameraClient *client_ = nullptr;
	QVector<Slot> slots_;
	QVector<QPushButton *> pageButtons_;
	QPushButton *presetModeBtn_ = nullptr;
	QPushButton *traceModeBtn_ = nullptr;
	QLabel *modeLabel_ = nullptr;
	/* Recall | Start-Record / Stop-Record. */
	QPushButton *primaryBtn_ = nullptr;
	/* Add-Replace | Prepare / Play / Stop-playback. */
	QPushButton *secondaryBtn_ = nullptr;
	QPushButton *deleteBtn_ = nullptr;
	QPushButton *renameBtn_ = nullptr;
	/* The RECALL SPEED row; presets only. */
	QWidget *speedRow_ = nullptr;
	QComboBox *unitCombo_ = nullptr;
	QComboBox *speedCombo_ = nullptr;
	QLabel *hintLabel_ = nullptr;

	/* Per absolute index (0..99): last get_info reply and its thumbnail. The
	   two families share the index space but not the entries, so switching
	   family invalidates both caches. */
	QHash<int, QJsonObject> info_;
	QHash<int, QPixmap> thumbs_;

	bool traceMode_ = false;
	/* Auto framing currently drives the head: only the trace actions grey. */
	bool framingOwned_ = false;
	/* `ptz_speed_mode` as the camera last reported it. Latched rather than
	   read off a widget for the same reason as zoomLockOn_: a gating rule that
	   reads the control it just wrote latches (bug 74 — the per-preset speed
	   row came back to life after every re-selection). */
	bool speedModeKnown_ = false;
	bool speedModeCommon_ = false;
	/* The one-shot read behind those two. */
	bool speedModeAsked_ = false;
	/* Last /ctrl/ptrace st_code; only meaningful in trace mode. */
	int traceState_ = 0;

	/* While the camera's trace transport is busy (recording, preparing or
	   playing), the UI polls its state so it self-transitions when the camera
	   finishes on its own — a finished playback must return the button to
	   "Prepare" without the operator clicking anything (bug 24). Owned here so
	   the panel can start/stop it as the transport state changes. */
	QTimer *tracePollTimer_ = nullptr;

	int page_ = 0;
	int selected_ = -1;
	int maxSpeed_ = 0;
	int maxTime_ = 0;
};

/* Main dock. */
class ZcControlDock : public QDockWidget {
	Q_OBJECT
public:
	explicit ZcControlDock(QWidget *parent = nullptr);
	~ZcControlDock() override;

	/* Bug 206: a double click on the title bar maximizes / restores, the way it
	   does on any other window. A floating QDockWidget is a real window with a
	   native caption (and no maximize box), so that click never reaches Qt's
	   event system at all — it is taken in nativeEvent(). The docked state has
	   no caption of its own; its double click is a client event on the dock
	   widget and is taken in eventFilter(). Both end in toggleMaximized(). */
	bool eventFilter(QObject *watched, QEvent *event) override;
	bool nativeEvent(const QByteArray &eventType, void *message,
			 qintptr *result) override;

	/* Connect the control client to `host`. Returns false — and leaves no
	   client behind, so nothing can be operated — when the camera does not
	   answer (spec §1, L11). */
	bool openCamera(const QString &host);
	/* Same, with Digest credentials (bug 4). */
	bool openCamera(const QString &host, const ZcCredentials &creds);

	/* Re-evaluate every rail row's "already added" badge and the Add button.
	   Public because the libobs source create/destroy/rename signals reach it
	   through a file-scope callback. Runs on the Qt main thread. */
	void refreshRailPresence();

	/* Select the camera behind an OBS source (spec §1, L2): the source's
	   Properties dialog opens the dock for the camera that source represents, so
	   the selected host must follow the source the operator clicked, not stay on
	   the last one they opened (bug 27). A no-op when the host is empty or is
	   already current. Runs on the Qt main thread. */
	void selectCamera(const QString &host);

private slots:
	void onCameraClicked(const QString &host);
	void refreshCameras();
	/* The rail header's rescan button: force an mDNS query now, re-read the
	   records at once (so the operator sees the current list immediately) and
	   once more after the forced query's replies have had time to land. */
	void onCameraRefreshRequested();
	/* The in-page login form was submitted: store the credentials for the host
	   and reconnect (bug 4). */
	void onLoginSubmitted(const QString &user, const QString &password);
	/* A setting write that restarts the camera: show a notice and start polling
	   for it to come back (bug 8). */
	void onCameraRestarting();
	/* One poll tick of the restart wait. */
	void tryReconnect();

private:
	/* The body of openCamera(): connects a fresh client and, on failure, tears it
	   down again. Split out so openCamera() can hold the re-entrancy guard around
	   it (see opening_ below). */
	bool openCameraNow(const QString &host, const ZcCredentials &creds);
	/* Opens the camera source's Interact window — the only pane libobs
	   forwards mouse events to, so the only surface a click-to-move or a
	   detection-area drag can happen on (see zc_source_bridge.h). */
	void openSourceInteraction();
	/* Closes it again. There is no frontend call for this, so the window is
	   found through the widget tree; see the comment on the definition. */
	void closeSourceInteraction();
	/* Maximize the panel if it is not maximized, restore it otherwise (bug
	   206). Both double-click paths (nativeEvent for the floating window,
	   eventFilter for the docked one) end here. */
	void toggleMaximized();
	/* Create/reuse an obs-zcamera source pointing at `host` and add it to the
	   current scene. Runs on the OBS frontend (Qt main) thread. */
	void addCameraToSource(const QString &host);
	/* Remove the "ZCamera <host>" source from OBS (all scenes). No-op when the
	   source does not exist. Runs on the OBS frontend thread. */
	void removeCameraFromSource(const QString &host);
	/* OBS source name that represents a camera: "ZCamera <ip>" (spec §1, L3). */
	static QString sourceNameForHost(const QString &host);
	/* Whether OBS currently holds a source for this camera (spec §1, L4). */
	static bool isCameraAdded(const QString &host);
	/* Show the PTZ and auto-framing tabs only for a camera that reports PTZ
	   support (spec §3). */
	void updatePtzTab();
	/* libobs source create/destroy/rename handler; hops to the dock thread. */
	static void onObsSourceSignal(void *data, calldata_t *params);
	/* Every host OBS holds as a `ZCamera <ip>` source, so the rail can list a
	   configured camera that discovery is not advertising (spec §1, L11). */
	static QSet<QString> sourceHosts();

	ZcCameraRail *rail_ = nullptr;
	ZcSettingsPanel *settings_ = nullptr;
	ZcPtzPad *ptz_ = nullptr;
	ZcPresetPanel *presets_ = nullptr;
	ZcFramingPanel *framing_ = nullptr;
	/* Container for the PTZ tab (joystick + preset list); only inserted into
	   the tab bar when the camera supports PTZ. */
	QWidget *ptzTab_ = nullptr;
	/* The PTZ pad scrolls inside this (it carries the joystick, the position
	   read-out and the range-limit rows). */
	QScrollArea *ptzScroll_ = nullptr;
	/* Container for the auto-framing tab, shown alongside the PTZ tab. */
	QWidget *framingTab_ = nullptr;
	QTabWidget *tabs_ = nullptr;
	ZcCameraClient *client_ = nullptr;
	/* The source <-> dock interaction link (click-to-move, detection-area
	   editing). Owned here, shared with the PTZ pad and the framing panel;
	   registered as the global pointer sink for its lifetime. */
	ZcOverlayLink *overlay_ = nullptr;
	QLabel *statusBarLabel_ = nullptr;
	/* Polls /ctrl/mode while connected (bug 167): firmware that never pushes
	   ModeChanged leaves the operator's own switch to playback invisible, so the
	   dock asks instead of waiting. Two seconds — the camera's HTTP server is
	   single-threaded and this must not compete with the panel reads. */
	QTimer *modePoll_ = nullptr;
	/* "● REC" while the camera records -- including the recording the camera
	   starts itself when tracking triggers it (bug list #20). */
	QLabel *recBadge_ = nullptr;
	QPushButton *debugButton_ = nullptr;
	ZcDebugPanel *debugPanel_ = nullptr;
	QSplitter *splitter_ = nullptr;
	QString currentHost_;
	/* Per-host Digest credentials entered for this session (bug 4). */
	QHash<QString, ZcCredentials> creds_;
	/* Whether the last openCamera() failure was a 401 rather than a timeout
	   (bug 4). */
	bool lastAuthRequired_ = false;
	/* Whether it failed because the camera only answers over HTTPS and this Qt
	   build cannot do TLS (the operator must switch the camera to HTTP). */
	bool lastTlsUnavailable_ = false;
	/* Hosts that answered a control connection although discovery does not
	   advertise them. Keeps such a row usable instead of marking it offline on
	   every refresh; a failed connect removes it again (spec §1, L11). */
	QSet<QString> reachableHosts_;
	/* Hosts whose last control connection failed. Keeps the offline mark on a
	   discovered camera that does not answer, instead of clearing it at the next
	   scan (spec §1, L11). */
	QSet<QString> unreachableHosts_;
	/* Every camera discovery has seen this session, address -> the name it
	   advertised under. A camera that stops advertising keeps its row, marked
	   offline, instead of vanishing from the rail; that is what makes a
	   powered-off camera clickable again once it comes back. Session-scoped, so
	   it is rebuilt from the network at the next launch. */
	QHash<QString, QString> knownCameras_;
	/* The cameras the previous scan saw advertised. A camera that re-appears
	   after being away has its stale "does not answer" mark dropped, so a
	   power cycle does not leave a reachable camera greyed out. */
	QSet<QString> advertised_;
	/* How many consecutive scans have come back with no camera at all. The rail
	   shows its busy spin for the first few (discovery needs a moment to be
	   answered) and the plain "no camera" line after that, so a permanently
	   empty rail does not sweep forever. Reset by any scan that lists one. */
	int railEmptyScans_ = 0;
	/* Re-entrancy guard: ZcCameraClient::connect() runs a nested event loop (the
	   origin probe), so openCamera() — and onCameraClicked() under it — can be
	   re-entered from an event delivered while a connect is in flight. A second
	   entry would run `delete client_` while the first client's connect() frame
	   is still on the stack, freeing its transport mid-call (crash in
	   negotiateOrigin at the base_url_ build). While true, a second request is
	   remembered instead of served. */
	bool opening_ = false;
	/* A camera pick that arrived while opening_; served once opening_ clears. */
	bool pendingClick_ = false;
	/* Rejected logins for the current camera (bug list: the camera locks the
	   account after 5 wrong passwords). */
	int loginAttempts_ = 0;
	QString pendingClickHost_;
	/* mDNS discovery refresh. */
	QTimer *scanTimer_ = nullptr;
	/* The manual-rescan window opened by the rail header's button: re-reads the
	   discovery records and re-asks while it runs, then clears the rail's spin
	   (see onCameraRefreshRequested). */
	QTimer *rescanTimer_ = nullptr;
	int rescanTicks_ = 0;
	/* Polls for a camera that a setting write made restart (bug 8). */
	QTimer *restartTimer_ = nullptr;
	int restartAttempts_ = 0;
	/* libobs global signal handler, for source create/destroy/rename. */
	signal_handler_t *obsSignals_ = nullptr;
};

} // namespace zc