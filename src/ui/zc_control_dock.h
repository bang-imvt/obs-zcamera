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
class QFrame;
class QBoxLayout;
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

	QListWidget *list_ = nullptr;
	QSet<QString> added_;
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

public slots:
	/* Re-read the stream bar and the visible group from the camera. */
	void refreshGroup();

signals:
	/* A write the camera applies by restarting (HTTPS, identity auth) or by
	   moving to another address (network type) was issued: the control
	   connection is about to go away until the camera is back (bug 8). */
	void cameraRestarting();

private slots:
	/* A catalog finished loading; rebuild the rows when it is the visible one. */
	void onGroupRefreshed(const QString &catalog);
	void onGroupSelected(const QString &catalog);

private:
	void populateGroups();
	void buildRows(const QString &catalog);
	/* The static Ethernet address block shown under the network group
	   (bug 15): /ctrl/network is not a settings catalog. */
	void buildStaticNetworkBlock(QBoxLayout *rowLayout);
	/* Write one setting and, when the camera applies it by restarting, tell the
	   dock so it can wait for the camera and reconnect (bug 8). */
	void writeKey(const QString &key, const QString &value);

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
	/* The catalog currently rendered, so a late getbatch reply knows whether
	   it still owns the rows pane. */
	QString catalog_;

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

	/* Recompute joystick geometry from the (fixed-size) joystick area. */
	void updateGeometry();
	/* Sector index (0..7) at widget pos, or -1 if outside the ring. */
	int directionAt(const QPoint &pos) const;
	bool inCenter(const QPoint &pos) const;
	QPainterPath sectorPath(int i) const;
	void moveInDirection(int dirIndex);
	void moveAnalog(const QPointF &offset);
	void stopMoving();

	ZcCameraClient *client_ = nullptr;
	QWidget *joystickArea_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QPushButton *homeButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;

	QRectF joystickRect_;
	QPointF joyCenter_;
	qreal joyOuterRadius_ = 96.0;
	qreal joyInnerRadius_ = 44.0;

	JoystickMode mode_ = JoystickMode::None;
	int activeDir_ = -1;
	QPointF dragOffset_;
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
	/* Ask the operator for the camera's login and retry; true when the camera
	   then answered (bug 4). */
	bool promptForCredentials(const QString &host);

	/* Re-evaluate every rail row's "already added" badge and the Add button.
	   Public because the libobs source create/destroy/rename signals reach it
	   through a file-scope callback. Runs on the Qt main thread. */
	void refreshRailPresence();

private slots:
	void onCameraClicked(const QString &host);
	void refreshCameras();
	/* A setting write that restarts the camera: show a notice and start polling
	   for it to come back (bug 8). */
	void onCameraRestarting();
	/* One poll tick of the restart wait. */
	void tryReconnect();

private:
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
	/* Show the PTZ tab only for a camera that reports PTZ support (spec §3). */
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
	/* Container for the PTZ tab (joystick + preset list); only inserted into
	   the tab bar when the camera supports PTZ. */
	QWidget *ptzTab_ = nullptr;
	QTabWidget *tabs_ = nullptr;
	ZcCameraClient *client_ = nullptr;
	QLabel *statusBarLabel_ = nullptr;
	QPushButton *debugButton_ = nullptr;
	ZcDebugPanel *debugPanel_ = nullptr;
	QSplitter *splitter_ = nullptr;
	QString currentHost_;
	/* Per-host Digest credentials entered for this session (bug 4). */
	QHash<QString, ZcCredentials> creds_;
	/* Whether the last openCamera() failure was a 401 rather than a timeout
	   (bug 4). */
	bool lastAuthRequired_ = false;
	/* Hosts that answered a control connection although discovery does not
	   advertise them. Keeps such a row usable instead of marking it offline on
	   every refresh; a failed connect removes it again (spec §1, L11). */
	QSet<QString> reachableHosts_;
	/* Hosts whose last control connection failed. Keeps the offline mark on a
	   discovered camera that does not answer, instead of clearing it at the next
	   scan (spec §1, L11). */
	QSet<QString> unreachableHosts_;
	/* mDNS discovery refresh. */
	QTimer *scanTimer_ = nullptr;
	/* Polls for a camera that a setting write made restart (bug 8). */
	QTimer *restartTimer_ = nullptr;
	int restartAttempts_ = 0;
	/* libobs global signal handler, for source create/destroy/rename. */
	signal_handler_t *obsSignals_ = nullptr;
};

} // namespace zc