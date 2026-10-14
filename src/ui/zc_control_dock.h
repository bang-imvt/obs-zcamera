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
#include <QJsonObject>
#include <QHash>
#include <QPixmap>
#include <QVector>

/* ZcCredentials, held per host for the session (bug 4). */
#include "zc_http_transport.h"

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

	/* The subset of the listed hosts that already exist in OBS as a source.
	   Rows for those hosts get the "added" badge and their chip turns into a
	   gold check instead of "＋" (spec §1, L4/L5). */
	void setAddedHosts(const QSet<QString> &added);

	/* Wait, scanning: replace the (empty) list with a busy page. Set while
	   discovery has not produced a single row yet, so an empty rail reads as
	   work in progress rather than as a scan that found nothing. */
	void setBusy(bool busy);

signals:
	void cameraClicked(const QString &host);
	/* A row's chip was clicked on a camera that is not yet a source: add that
	   camera to the current scene. The chip acts on its own row, so no selection
	   is needed first (spec §1, L6). */
	void addRequested(const QString &host);
	/* A row's chip was clicked on a camera that IS already a source: remove the
	   source from OBS (spec §1, L5). */
	void removeRequested(const QString &host);

private:
	/* Compose the fields, glyph and tooltip of one row from its stored fields.
	   The label itself is painted by ZcRailDelegate. */
	void applyRow(QListWidgetItem *item);
	/* Show exactly one of the three pages: the list once it has rows, the
	   busy spin while scanning has yet to name one, and the "no camera" line
	   once scanning has given up. */
	void updateEmptyState();

	QListWidget *list_ = nullptr;
	QSet<QString> added_;
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
class ZcPtzPad : public QWidget {
	Q_OBJECT
public:
	explicit ZcPtzPad(QWidget *parent = nullptr);

	void setClient(ZcCameraClient *client);

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
	/* Set one edge to the head's current position (the web UI's "取当前位"). */
	void setEdgeToCurrent(Edge edge);

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
	QWidget *joystickArea_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QPushButton *homeButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;

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
	QPushButton *limitCurrent_[EdgeCount] = {};
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

public slots:
	/* Re-read the framing snapshot and the preset list, then repaint. */
	void refresh();
	/* Paint from a framing snapshot (/ctrl/framing/all_status) or a live
	   FramingStatusChanged payload: only the fields present are applied. */
	void applyStatus(const QJsonObject &doc);

private:
	/* Re-read /ctrl/framing/preset and rebuild the tile list. */
	void refreshPresets();
	void rebuildPresets(const QJsonObject &doc);
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
	/* Move one /ctrl/framing/fov offset by one step and tell the camera. Only
	   ever called from the framing pad; the read-out follows the write. */
	void nudgeOffset(bool placement, int delta);
	/* Repaint the two-offset read-out under the framing pad. */
	void updatePadReadOut();
	/* Open a projector window on this camera's own source. That window is one
	   of the few views that renders the source by itself, so it is where a
	   monitor-only detection overlay can be watched without any of it reaching
	   the recording — the standard preview pane blits the program mix instead
	   and therefore cannot show it. Falls back to a window on the current
	   scene when this camera has no source in OBS yet. */
	void openMonitorWindow();

	/* The camera's page is three live buttons, not a row of checkboxes:
	   Enable/Disable toggles `detect`, Start/Stop toggles `trace`, and Restart
	   re-runs detection. They act immediately, as on the page. */
	QPushButton *enableBtn_ = nullptr;
	QPushButton *startBtn_ = nullptr;
	QPushButton *restartBtn_ = nullptr;
	/* Opens the projector window described on openMonitorWindow(). */
	QPushButton *monitorBtn_ = nullptr;
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

	/* The page's dropdowns. */
	QComboBox *viewAngleCombo_ = nullptr; /* Live: View Angle (fov_type) */
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

	/* The framing pad and the read-out of the two offsets it changes. */
	ZcFramingPad *pad_ = nullptr;
	QLabel *padReadOut_ = nullptr;

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
	/* Set once the optional-feature probe has run for the current camera, so a
	   refresh does not re-ask on every tick. */
	bool optionalProbed_ = false;

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
	void beginRename();
	void commitRename(int index, const QString &text);
	/* Re-read the /ctrl/ptrace transport state, then re-enable the buttons. */
	void refreshTraceState();
	void onTraceState(int stCode);
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
	QLabel *statusBarLabel_ = nullptr;
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
	/* Polls for a camera that a setting write made restart (bug 8). */
	QTimer *restartTimer_ = nullptr;
	int restartAttempts_ = 0;
	/* libobs global signal handler, for source create/destroy/rename. */
	signal_handler_t *obsSignals_ = nullptr;
};

} // namespace zc