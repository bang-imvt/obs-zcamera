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

#include "zc_control_dock.h"
#include "zc_debug_panel.h"
#include "zc_camera_client.h"
#include "zc_settings_schema.h"

/* Must come before the OBS headers: ssp-mdns.h pulls in winsock2 via <mdns.h>,
   which has to be included before windows.h (brought in by obs-module.h). */
#include "ssp-mdns.h"

#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QStackedWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QGroupBox>
#include <QFrame>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QInputDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <functional>
#include <array>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTimer>
#include <QIcon>
#include <QPixmap>
#include <QSet>
#include <cmath>
#include <algorithm>

namespace zc {

/* Translate a dock string through the OBS locale (.ini). The dock is plain Qt
   widgets with no QTranslator installed, so obs_module_text is what picks the
   language OBS runs in. Values read back from the camera (names, option labels,
   numbers) are never passed through here. */
static QString ztr(const char *key)
{
	/* obs_module_text returns the .ini value verbatim; a literal "\n" in a
	   value (used to open a second tooltip line) is turned into a real
	   newline here. */
	return QString::fromUtf8(obs_module_text(key)).replace(
		QStringLiteral("\\n"), QStringLiteral("\n"));
}

/* Human label for a settings row. The camera reports lowercase ids
   ("http_auth"); the locale turns the ones whose id is misleading into readable
   text (bug 5: `http_auth` is the identity-authentication switch, not a second
   HTTPS toggle). A key with no entry keeps its raw id, so a key a newer
   firmware adds is still usable. */
static QString settingLabel(const QString &key)
{
	/* Readable labels following ZCamGuiOpen's settings-schema labels; a key
	   without an entry shows its raw id. The names are kept aligned with the
	   camera's own web UI (bug list "unify setting names"), and every schema key
	   has an entry in the .ini so the row is translated, not a raw camera id
	   (bug list "settings not translated to Chinese"). */
	static const QHash<QString, const char *> kLabels = {
		/* exposure */
		{QStringLiteral("http_auth"), "ZCameraPlugin.Setting.HttpAuth"},
		{QStringLiteral("https_on"), "ZCameraPlugin.Setting.HttpsOn"},
		{QStringLiteral("ev"), "ZCameraPlugin.Setting.Ev"},
		{QStringLiteral("ev_choice"), "ZCameraPlugin.Setting.EvChoice"},
		{QStringLiteral("shutter"), "ZCameraPlugin.Setting.Shutter"},
		{QStringLiteral("max_shutter"), "ZCameraPlugin.Setting.MaxShutter"},
		{QStringLiteral("sht_operation"),
		 "ZCameraPlugin.Setting.ShutterOperation"},
		{QStringLiteral("flicker"), "ZCameraPlugin.Setting.Flicker"},
		{QStringLiteral("meter_mode"), "ZCameraPlugin.Setting.MeterMode"},
		{QStringLiteral("iris"), "ZCameraPlugin.Setting.Iris"},
		{QStringLiteral("iso"), "ZCameraPlugin.Setting.Iso"},
		{QStringLiteral("min_iso"), "ZCameraPlugin.Setting.MinIso"},
		{QStringLiteral("max_iso"), "ZCameraPlugin.Setting.MaxIso"},
		{QStringLiteral("iso_ctrl"), "ZCameraPlugin.Setting.IsoCtrl"},
		{QStringLiteral("shutter_angle_ctrl"),
		 "ZCameraPlugin.Setting.ShutterAngleCtrl"},
		{QStringLiteral("eND"), "ZCameraPlugin.Setting.End"},
		{QStringLiteral("lock_ae_in_rec"), "ZCameraPlugin.Setting.LockAeInRec"},
		{QStringLiteral("ae_speed"), "ZCameraPlugin.Setting.AeSpeed"},
		{QStringLiteral("bl_comp"), "ZCameraPlugin.Setting.BlComp"},
		/* white balance */
		{QStringLiteral("wb"), "ZCameraPlugin.Setting.Wb"},
		{QStringLiteral("wb_priority"), "ZCameraPlugin.Setting.WbPriority"},
		{QStringLiteral("lock_awb_in_rec"),
		 "ZCameraPlugin.Setting.LockAwbInRec"},
		{QStringLiteral("mwb"), "ZCameraPlugin.Setting.Mwb"},
		{QStringLiteral("tint"), "ZCameraPlugin.Setting.Tint"},
		{QStringLiteral("mwb_r"), "ZCameraPlugin.Setting.MwbR"},
		{QStringLiteral("mwb_g"), "ZCameraPlugin.Setting.MwbG"},
		{QStringLiteral("mwb_b"), "ZCameraPlugin.Setting.MwbB"},
		/* lens / focus */
		{QStringLiteral("focus"), "ZCameraPlugin.Setting.Focus"},
		{QStringLiteral("af_mode"), "ZCameraPlugin.Setting.AfMode"},
		{QStringLiteral("caf"), "ZCameraPlugin.Setting.Caf"},
		{QStringLiteral("caf_sens"), "ZCameraPlugin.Setting.CafSens"},
		{QStringLiteral("af_area"), "ZCameraPlugin.Setting.AfArea"},
		{QStringLiteral("live_caf"), "ZCameraPlugin.Setting.LiveCaf"},
		{QStringLiteral("mf_mag"), "ZCameraPlugin.Setting.MfMag"},
		{QStringLiteral("mf_recording"), "ZCameraPlugin.Setting.MfRecording"},
		{QStringLiteral("af_speed"), "ZCameraPlugin.Setting.AfSpeed"},
		{QStringLiteral("af_adjust_with_ptz"),
		 "ZCameraPlugin.Setting.AfAdjustWithPtz"},
		{QStringLiteral("zoom_mode"), "ZCameraPlugin.Setting.ZoomMode"},
		{QStringLiteral("lens_focus_pos"),
		 "ZCameraPlugin.Setting.LensFocusPos"},
		{QStringLiteral("lens_zoom_pos"), "ZCameraPlugin.Setting.LensZoomPos"},
		{QStringLiteral("fz_speed"), "ZCameraPlugin.Setting.FzSpeed"},
		/* image */
		{QStringLiteral("lut"), "ZCameraPlugin.Setting.Lut"},
		{QStringLiteral("sharpness"), "ZCameraPlugin.Setting.Sharpness"},
		{QStringLiteral("noise_reduction"),
		 "ZCameraPlugin.Setting.NoiseReduction"},
		{QStringLiteral("luma_level"), "ZCameraPlugin.Setting.LumaLevel"},
		{QStringLiteral("brightness"), "ZCameraPlugin.Setting.Brightness"},
		{QStringLiteral("contrast"), "ZCameraPlugin.Setting.Contrast"},
		{QStringLiteral("saturation"), "ZCameraPlugin.Setting.Saturation"},
		{QStringLiteral("hue"), "ZCameraPlugin.Setting.Hue"},
		/* record */
		{QStringLiteral("resolution"), "ZCameraPlugin.Setting.Resolution"},
		{QStringLiteral("project_fps"), "ZCameraPlugin.Setting.ProjectFps"},
		{QStringLiteral("vfr_ctrl"), "ZCameraPlugin.Setting.VfrCtrl"},
		{QStringLiteral("movvfr"), "ZCameraPlugin.Setting.MovVfr"},
		{QStringLiteral("record_file_format"),
		 "ZCameraPlugin.Setting.RecordFileFormat"},
		{QStringLiteral("rotation"), "ZCameraPlugin.Setting.Rotation"},
		{QStringLiteral("split_duration"),
		 "ZCameraPlugin.Setting.SplitDuration"},
		{QStringLiteral("rec_fps"), "ZCameraPlugin.Setting.RecFps"},
		{QStringLiteral("preroll"), "ZCameraPlugin.Setting.Preroll"},
		{QStringLiteral("video_tl_interval"),
		 "ZCameraPlugin.Setting.VideoTlInterval"},
		{QStringLiteral("record_meta"), "ZCameraPlugin.Setting.RecordMeta"},
		{QStringLiteral("camera_id"), "ZCameraPlugin.Setting.CameraId"},
		{QStringLiteral("reelname"), "ZCameraPlugin.Setting.ReelName"},
		/* video */
		{QStringLiteral("video_encoder"),
		 "ZCameraPlugin.Setting.VideoEncoder"},
		{QStringLiteral("bitrate_level"), "ZCameraPlugin.Setting.BitrateLevel"},
		{QStringLiteral("record_mode"), "ZCameraPlugin.Setting.RecordMode"},
		{QStringLiteral("rec_proxy_file"),
		 "ZCameraPlugin.Setting.RecProxyFile"},
		{QStringLiteral("crop_sensor"), "ZCameraPlugin.Setting.CropSensor"},
		{QStringLiteral("low_jello"), "ZCameraPlugin.Setting.LowJello"},
		{QStringLiteral("photo_q"), "ZCameraPlugin.Setting.PhotoQ"},
		{QStringLiteral("eis_on_off"), "ZCameraPlugin.Setting.EisOnOff"},
		{QStringLiteral("vid_rot"), "ZCameraPlugin.Setting.VidRot"},
		/* audio */
		{QStringLiteral("primary_audio"), "ZCameraPlugin.Setting.PrimaryAudio"},
		{QStringLiteral("audio_channel"), "ZCameraPlugin.Setting.AudioChannel"},
		{QStringLiteral("audio_phantom_power"),
		 "ZCameraPlugin.Setting.AudioPhantomPower"},
		{QStringLiteral("audio_level_display"),
		 "ZCameraPlugin.Setting.AudioLevelDisplay"},
		{QStringLiteral("ain_gain_type"), "ZCameraPlugin.Setting.InputGainType"},
		{QStringLiteral("audio_input_level"),
		 "ZCameraPlugin.Setting.AudioInputLevel"},
		{QStringLiteral("audio_input_gain"),
		 "ZCameraPlugin.Setting.AudioInputGain"},
		{QStringLiteral("audio_noise_reduction"),
		 "ZCameraPlugin.Setting.AudioNoiseReduction"},
		{QStringLiteral("audio_in_l_gain"), "ZCameraPlugin.Setting.AudioInLGain"},
		{QStringLiteral("audio_in_r_gain"), "ZCameraPlugin.Setting.AudioInRGain"},
		{QStringLiteral("audio_output_gain"),
		 "ZCameraPlugin.Setting.AudioOutputGain"},
		/* ptz */
		{QStringLiteral("pt_speedmode"), "ZCameraPlugin.Setting.PtSpeedmode"},
		{QStringLiteral("ptz_flip"), "ZCameraPlugin.Setting.PtzFlip"},
		{QStringLiteral("ptz_limit"), "ZCameraPlugin.Setting.PtzLimit"},
		{QStringLiteral("ptz_preset_mode"),
		 "ZCameraPlugin.Setting.PtzPresetMode"},
		{QStringLiteral("ptz_speed_mode"),
		 "ZCameraPlugin.Setting.PtzSpeedMode"},
		{QStringLiteral("freeze_during_preset"),
		 "ZCameraPlugin.Setting.FreezeDuringPreset"},
		{QStringLiteral("pt_pwr_pos"), "ZCameraPlugin.Setting.PtPwrPos"},
		{QStringLiteral("pt_priv_mode"), "ZCameraPlugin.Setting.PtPrivMode"},
		{QStringLiteral("preset_adjust_speed_with_zoom"),
		 "ZCameraPlugin.Setting.PresetAdjustSpeedWithZoom"},
		{QStringLiteral("pt_speed_with_zoom_pos"),
		 "ZCameraPlugin.Setting.PtSpeedWithZoomPos"},
		{QStringLiteral("ptz_common_speed_unit"),
		 "ZCameraPlugin.Setting.PtzCommonSpeedUnit"},
		{QStringLiteral("ptz_common_speed"),
		 "ZCameraPlugin.Setting.PtzCommonSpeed"},
		{QStringLiteral("ptz_common_time"),
		 "ZCameraPlugin.Setting.PtzCommonTime"},
	};
	auto it = kLabels.constFind(key);
	if (it == kLabels.constEnd())
		return key;
	/* obs_module_text echoes the key itself when the locale has no entry. */
	const QString text = ztr(*it);
	return text == QLatin1String(*it) ? key : text;
}

/* Human label for a settings catalog (the sidebar's group tabs). The camera
   exposes lowercase ids ("wb"); the locale maps each to a readable title, and
   an id with no entry keeps its raw form, so a catalog a newer firmware adds is
   still listed. The id itself stays in Qt::UserRole, never shown. */
static QString catalogLabel(const QString &id)
{
	static const QHash<QString, const char *> kLabels = {
		{QStringLiteral("exposure"), "ZCameraPlugin.Catalog.Exposure"},
		{QStringLiteral("wb"), "ZCameraPlugin.Catalog.Wb"},
		{QStringLiteral("lens"), "ZCameraPlugin.Catalog.Lens"},
		{QStringLiteral("image"), "ZCameraPlugin.Catalog.Image"},
		{QStringLiteral("record"), "ZCameraPlugin.Catalog.Record"},
		{QStringLiteral("video"), "ZCameraPlugin.Catalog.Video"},
		{QStringLiteral("audio"), "ZCameraPlugin.Catalog.Audio"},
		{QStringLiteral("ptz"), "ZCameraPlugin.Catalog.Ptz"},
	};
	auto it = kLabels.constFind(id);
	if (it == kLabels.constEnd())
		return id;
	/* obs_module_text echoes the key itself when the locale has no entry. */
	const QString text = ztr(*it);
	return text == QLatin1String(*it) ? id : text;
}

/* Small painted camera glyph so the discovery list needs no bundled asset.
   With `added` a gold check badge is overlaid, marking a camera that already
   exists in OBS as a source (spec §1). With `offline` the body is drawn in the
   muted colour: the camera is only known from its source, discovery is not
   advertising it and it does not answer, so it cannot be controlled. */
static QIcon zcCameraIcon(bool added = false, bool offline = false)
{
	QPixmap pm(24, 24);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	/* body + lens in the dock's accent colour. */
	p.setPen(Qt::NoPen);
	p.setBrush(offline ? QColor("#6b7a90") : QColor("#9db8d2"));
	p.drawRoundedRect(QRectF(2, 9, 20, 11), 2, 2);
	p.drawRect(QRectF(8, 6, 6, 4)); /* top bump */
	p.setBrush(QColor("#1c2733"));
	p.drawEllipse(QRectF(7, 12, 10, 5)); /* lens */

	if (added) {
		/* Gold disc + check, bottom-right. */
		p.setBrush(QColor("#d8b878"));
		p.setPen(Qt::NoPen);
		p.drawEllipse(QRectF(14, 14, 10, 10));
		QPen pen(QColor("#1f2927"));
		pen.setWidthF(1.8);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.drawLine(QPointF(16.6, 19.2), QPointF(18.6, 21.2));
		p.drawLine(QPointF(18.6, 21.2), QPointF(21.6, 17.4));
	}
	p.end();
	return QIcon(pm);
}

/* ---------------------------------------------------------------- */
/* ZcCameraRail                                                      */
/* ---------------------------------------------------------------- */

/* Per-row fields. The host doubles as the row's identity everywhere else. */
static constexpr int kHostRole = Qt::UserRole;
static constexpr int kNameRole = Qt::UserRole + 1;
static constexpr int kOfflineRole = Qt::UserRole + 2;
static constexpr int kAddedRole = Qt::UserRole + 3;

/* Row geometry: two lines of text, and room for the action chip on the right. */
static constexpr int kRailRowPadV = 5;
static constexpr int kRailChipW = 26;
static constexpr int kRailChipH = 20;

/* Paints one rail row as two lines — the camera's name above its address — and
   hit-tests the row's own action chip.

   The chip is the row's "add to the current scene" control (spec §1 L5/L6), so
   the rail has no Add button of its own: the operator acts on the camera they
   are looking at instead of having to select it first. It cannot be a
   QPushButton, because a QListWidget row has no widget of its own and an item
   widget would swallow the click that selects the row; it is painted and
   hit-tested here instead. */
class ZcRailDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	/* Called with the row's host and whether it is already an OBS source when
	   its chip is clicked (a gold check removes the source, "＋" adds it). */
	std::function<void(const QString &host, bool added)> onChip;

	QSize sizeHint(const QStyleOptionViewItem &option,
		       const QModelIndex &index) const override
	{
		Q_UNUSED(index);
		return QSize(150, option.fontMetrics.height() * 2 + kRailRowPadV * 2);
	}

	/* The chip's rect inside an item's rect. */
	static QRect chipRect(const QRect &itemRect)
	{
		return QRect(itemRect.right() - kRailChipW - 4,
			     itemRect.center().y() - kRailChipH / 2, kRailChipW,
			     kRailChipH);
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option,
		   const QModelIndex &index) const override
	{
		QStyleOptionViewItem opt(option);
		initStyleOption(&opt, index);
		/* The base class draws the row background, selection and focus; the
		   text and the glyph are drawn below so the row is two lines. */
		opt.text.clear();
		opt.icon = QIcon();
		const QWidget *widget = opt.widget;
		QStyle *style = widget ? widget->style() : QApplication::style();
		style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

		const QString host = index.data(kHostRole).toString();
		const QString name = index.data(kNameRole).toString();
		const bool offline = index.data(kOfflineRole).toBool();
		const bool added = index.data(kAddedRole).toBool();

		/* The camera the dock is controlling carries the gold mark (spec §1). */
		if (opt.state & QStyle::State_Selected)
			painter->fillRect(QRect(option.rect.left(), option.rect.top(), 3,
						option.rect.height()),
					  QColor("#d8b878"));

		const QRect content =
		    option.rect.adjusted(8, kRailRowPadV, -(kRailChipW + 10), -kRailRowPadV);
		const int glyphW = 24;
		const QRect glyph(content.left(), content.center().y() - glyphW / 2,
				  glyphW, glyphW);
		index.data(Qt::DecorationRole).value<QIcon>().paint(painter, glyph);

		const QRect text = content.adjusted(glyphW + 6, 0, 0, 0);
		const int lineH = text.height() / 2;
		/* Name above, address below: the address is what identifies the OBS
		   source ("ZCamera <ip>", spec §1 L3), and on one line the two made a
		   long label that was hard to scan. A row known only from a source has
		   no name, so its first line is the address. */
		const QString line1 = name.isEmpty() ? host : name;
		const QString line2 = name.isEmpty()
					      ? (offline ? ztr("ZCameraPlugin.Dock.Offline")
							 : ztr("ZCameraPlugin.Dock.NotDiscovered"))
					      : host + (offline ? ztr("ZCameraPlugin.Dock.OfflineMark") : QString());
		const QFontMetrics fm(option.font);
		painter->setFont(option.font);
		painter->setPen(offline ? QColor("#6b7a90") : QColor("#eef2ed"));
		painter->drawText(QRect(text.left(), text.top(), text.width(), lineH),
				  Qt::AlignLeft | Qt::AlignVCenter,
				  fm.elidedText(line1, Qt::ElideRight, text.width()));
		painter->setPen(offline ? QColor("#6b7a90") : QColor("#8a9bb2"));
		painter->drawText(
		    QRect(text.left(), text.top() + lineH, text.width(), lineH),
		    Qt::AlignLeft | Qt::AlignVCenter,
		    fm.elidedText(line2, Qt::ElideRight, text.width()));

		/* The action chip: "＋" while the camera can still be added, a gold
		   check once it is an OBS source (spec §1 L5). */
		const QRect chip = chipRect(option.rect);
		painter->setRenderHint(QPainter::Antialiasing, true);
		painter->setPen(QPen(added ? QColor("#3c514e") : QColor("#d8b878")));
		painter->setBrush(added ? QColor("#232d2b") : QColor("#2b3a38"));
		painter->drawRoundedRect(chip, 3, 3);
		if (added) {
			QPen pen(QColor("#d8b878"));
			pen.setWidthF(1.8);
			pen.setCapStyle(Qt::RoundCap);
			painter->setPen(pen);
			const QPointF c = chip.center();
			painter->drawLine(QPointF(c.x() - 4.5, c.y()), QPointF(c.x() - 1.5, c.y() + 3));
			painter->drawLine(QPointF(c.x() - 1.5, c.y() + 3), QPointF(c.x() + 4.5, c.y() - 3.5));
		} else {
			painter->setPen(QColor("#eef2ed"));
			painter->drawText(chip, Qt::AlignCenter, QStringLiteral("＋"));
		}
	}

	bool editorEvent(QEvent *event, QAbstractItemModel *model,
			 const QStyleOptionViewItem &option,
			 const QModelIndex &index) override
	{
		Q_UNUSED(model);
		const QEvent::Type type = event->type();
		if (type != QEvent::MouseButtonPress &&
		    type != QEvent::MouseButtonRelease)
			return false;
		auto *me = static_cast<QMouseEvent *>(event);
		if (me->button() != Qt::LeftButton ||
		    !chipRect(option.rect).contains(me->pos()))
			return false;
		const QString host = index.data(kHostRole).toString();
		if (host.isEmpty())
			return false;
		/* The chip works on an added row too: a gold check on a camera that is
		   already a source removes it (spec §1, L5). */
		const bool added = index.data(kAddedRole).toBool();
		/* Fire exactly once per click. The press does the work and is remembered;
		   the matching release only clears that memory and must NOT fire again —
		   by the release the row may already show the new state (e.g. a just-added
		   camera now reads "added"), so re-firing would remove what was just
		   added. A release with no tracked press is the only case that fires from
		   release (a view that skips the press). The press always re-arms, so a
		   gesture whose release lands outside the chip — where this method is not
		   called at all — cannot leave the next click on that row swallowed. */
		if (type == QEvent::MouseButtonPress) {
			clickedHost_ = host;
			if (onChip)
				onChip(host, added);
		} else if (clickedHost_ == host) {
			clickedHost_.clear();
		} else if (onChip) {
			onChip(host, added);
		}
		return true;
	}

private:
	/* Host whose chip was pressed, so its release does not fire a second time. */
	mutable QString clickedHost_;
};

ZcCameraRail::ZcCameraRail(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Cameras"), this);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	layout->addWidget(title);

	list_ = new QListWidget(this);
	list_->setMinimumWidth(180);
	/* Each row carries its own action chip, so the click that acts on a camera
	   never has to go through a selection first (spec §1 L5/L6). */
	auto *delegate = new ZcRailDelegate(list_);
	delegate->onChip = [this](const QString &host, bool added) {
		if (added)
			emit removeRequested(host);
		else
			emit addRequested(host);
	};
	list_->setItemDelegate(delegate);
	layout->addWidget(list_, 1);

	connect(list_, &QListWidget::itemClicked, this,
		[this](QListWidgetItem *item) {
			QString host = item->data(kHostRole).toString();
			emit cameraClicked(host);
		});
}

/* Fields, glyph and tooltip of one row. The label itself is drawn by
   ZcRailDelegate; the item's text is kept for keyboard search only. */
void ZcCameraRail::applyRow(QListWidgetItem *item)
{
	const QString host = item->data(kHostRole).toString();
	const QString name = item->data(kNameRole).toString();
	const bool offline = item->data(kOfflineRole).toBool();
	const bool added = added_.contains(host);

	/* The OBS source for this camera is named "ZCamera <ip>" (spec §1 L3), so
	   the address belongs in the row: without it a rail entry
	   ("P2-R1-18xx") cannot be matched against the source list
	   ("ZCamera 192.168.10.120"), and two cameras of the same model are
	   indistinguishable. */
	item->setText(name.isEmpty()
			      ? host
			      : QStringLiteral("%1 %2").arg(name, host));
	item->setData(kAddedRole, added);
	item->setIcon(zcCameraIcon(added, offline));

	QString tip = QStringLiteral("ZCamera %1").arg(host);
	if (offline)
		tip += ztr("ZCameraPlugin.Dock.OfflineControl");
	tip += added ? ztr("ZCameraPlugin.Dock.ClickToRemove")
		     : ztr("ZCameraPlugin.Dock.ClickToAdd");
	item->setToolTip(tip);
}

/* Identity of a row as the rail draws it: address, offline mark and name. The
   name is part of it because a source-only row carries none until discovery
   starts advertising the camera, and that transition has to redraw the label. */
static QString rowKey(const QString &host, const QString &name, bool offline)
{
	return host + (offline ? "!" : "") + "|" + name;
}

void ZcCameraRail::setCameras(const QVector<ZcRailCamera> &cameras)
{
	/* Skip the rebuild when the same rows would be produced, so a periodic scan
	   neither drops the current selection nor makes the dock flicker. */
	QSet<QString> current;
	for (int i = 0; i < list_->count(); ++i) {
		const QListWidgetItem *item = list_->item(i);
		current.insert(rowKey(item->data(kHostRole).toString(),
				      item->data(kNameRole).toString(),
				      item->data(kOfflineRole).toBool()));
	}
	QSet<QString> next;
	for (const auto &c : cameras) {
		if (!c.host.isEmpty())
			next.insert(rowKey(c.host, c.name, c.offline));
	}
	if (current == next)
		return;

	/* Remember the highlighted row and put it back after the rebuild: clear()
	   replaces every item and would otherwise drop the operator's selection even
	   though the same camera is still in the list. setCurrentItem() does not
	   emit itemClicked, so this cannot re-open the camera. */
	const QString selectedHost =
	    list_->currentItem()
		? list_->currentItem()->data(kHostRole).toString()
		: QString();

	QListWidgetItem *toSelect = nullptr;
	list_->clear();
	for (const auto &c : cameras) {
		if (c.host.isEmpty())
			continue;
		auto *item = new QListWidgetItem();
		item->setData(kHostRole, c.host);
		item->setData(kNameRole, c.name);
		item->setData(kOfflineRole, c.offline);
		applyRow(item);
		list_->addItem(item);
		if (!selectedHost.isEmpty() && c.host == selectedHost)
			toSelect = item;
	}
	if (toSelect)
		list_->setCurrentItem(toSelect);
}

/* Spec §1, L11: a row's offline state can change on a click, and rebuilding the
   list for that would drop the selection the operator just made. */
void ZcCameraRail::setHostOffline(const QString &host, bool offline)
{
	for (int i = 0; i < list_->count(); ++i) {
		QListWidgetItem *item = list_->item(i);
		if (item->data(kHostRole).toString() != host)
			continue;
		if (item->data(kOfflineRole).toBool() == offline)
			return;
		item->setData(kOfflineRole, offline);
		applyRow(item);
		return;
	}
}

void ZcCameraRail::setAddedHosts(const QSet<QString> &added)
{
	if (added_ == added)
		return;
	added_ = added;
	/* Update the rows in place: rebuilding the list would drop the selection
	   (and the connected camera) on every OBS source change. */
	for (int i = 0; i < list_->count(); ++i)
		applyRow(list_->item(i));
}

/* ---------------------------------------------------------------- */
/* ZcHud                                                             */
/* ---------------------------------------------------------------- */

ZcHud::ZcHud(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_TransparentForMouseEvents, false);
	setStyleSheet("background:rgba(0,0,0,0);");

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);

	/* Top row: REC badge + mode. */
	auto *topRow = new QHBoxLayout;
	recBadge_ = new QLabel("● REC", this);
	recBadge_->setStyleSheet(
	    "color:#e53935;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;font-weight:bold;");
	recBadge_->hide();
	durationLabel_ = new QLabel("", this);
	durationLabel_->setStyleSheet(
	    "color:#fff;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	remainingLabel_ = new QLabel("", this);
	remainingLabel_->setStyleSheet(
	    "color:#8a9bb2;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	modeLabel_ = new QLabel("", this);
	modeLabel_->setStyleSheet(
	    "color:#eef2ed;background:rgba(0,0,0,.45);border-radius:3px;"
	    "padding:2px 8px;");
	topRow->addWidget(recBadge_);
	topRow->addWidget(durationLabel_);
	topRow->addWidget(remainingLabel_);
	topRow->addStretch();
	topRow->addWidget(modeLabel_);
	layout->addLayout(topRow);

	layout->addStretch();

	/* Bottom info chips row. */
	auto *infoRow = new QHBoxLayout;
	QStringList labels = {"ISO", "SHUTTER", "EV", "WB", "IRIS", "FPS",
			      "RES", "TEMP"};
	for (const QString &lbl : labels) {
		auto *chip = new QLabel(lbl + " —", this);
		chip->setStyleSheet(
		    "color:#eef2ed;background:rgba(0,0,0,.45);"
		    "border-radius:3px;padding:2px 8px;font-size:11px;");
		infoRow->addWidget(chip);
		chips_.append(chip);
	}
	infoRow->addStretch();
	layout->addLayout(infoRow);
}

void ZcHud::setRecording(bool rec)
{
	recBadge_->setVisible(rec);
}

void ZcHud::setMode(const QString &mode)
{
	modeLabel_->setText(mode);
}

void ZcHud::setTemperature(double celsius)
{
	if (celsius < 0)
		return;
	if (chips_.size() < 8)
		return;
	chips_[7]->setText("TEMP " +
			   QString::number(celsius, 'f', 1) + "°C");
}

void ZcHud::setInfo(const QString &iso, const QString &shutter,
		    const QString &ev, const QString &wb, const QString &iris,
		    const QString &fps, const QString &res, const QString &temp)
{
	QStringList vals = {iso, shutter, ev, wb, iris, fps, res, temp};
	QStringList labels = {"ISO", "SHUTTER", "EV", "WB", "IRIS", "FPS",
			      "RES", "TEMP"};
	for (int i = 0; i < chips_.size(); ++i) {
		QString v = i < vals.size() ? vals[i] : QString();
		QString label = labels[i];
		QString display = v.isEmpty() ? (label + " -") : (label + " " + v);
		chips_[i]->setText(display);
		chips_[i]->setProperty("label", label);
		if (!v.isEmpty()) {
			chips_[i]->setStyleSheet(
			    "color:rgba(216,184,120,.9);background:rgba(0,0,0,.45);"
			    "border-radius:3px;padding:2px 8px;font-size:11px;"
			    "font-weight:bold;");
		}
	}
}

void ZcHud::setDuration(int seconds)

{
	int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
	durationLabel_->setText(QString("%1:%2:%3")
					.arg(h, 2, 10, QChar('0'))
					.arg(m, 2, 10, QChar('0'))
					.arg(s, 2, 10, QChar('0')));
}

void ZcHud::setRemaining(int seconds)
{
	if (seconds <= 0) {
		remainingLabel_->clear();
		return;
	}
	int h = seconds / 3600, m = (seconds % 3600) / 60;
	remainingLabel_->setText(QString("%1h %2m").arg(h).arg(m));
}

/* ---------------------------------------------------------------- */
/* ZcSettingsPanel                                                   */
/* ---------------------------------------------------------------- */

ZcSettingsPanel::ZcSettingsPanel(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	auto *header = new QHBoxLayout;
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Settings"), this);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	header->addWidget(title);
	header->addStretch(1);
	auto *refresh = new QPushButton(ztr("ZCameraPlugin.Dock.Refresh"), this);
	refresh->setToolTip(ztr("ZCameraPlugin.Dock.RefreshTip"));
	refresh->setStyleSheet(
	    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
	    "#3c514e; border-radius:4px; padding:3px 10px; font-size:11px; }"
	    "QPushButton:hover { background:#354845; }");
	header->addWidget(refresh);
	layout->addLayout(header);

	/* The stream bar sits above the group list rather than in it: how the
	   camera encodes the network stream is the setting an operator reaches for
	   most, so it must be there without navigating to a group (spec §2). */
	buildStreamBar(layout);

	/* In-page login / HTTPS fallback: shown when the camera answers 401 (need a
	   login) or is HTTPS-only with no TLS backend; hidden once connected. */
	loginPage_ = new QWidget(this);
	{
		auto *lp = new QVBoxLayout(loginPage_);
		lp->setContentsMargins(8, 8, 8, 8);
		promptHint_ = new QLabel(loginPage_);
		promptHint_->setWordWrap(true);
		promptHint_->setStyleSheet(
		    "color:#eef2ed;background:#2b3a38;border-radius:4px;padding:8px;");
		lp->addWidget(promptHint_);
		/* Rejection notice: hidden until a login fails, then shown in red under
		   the hint so a wrong user/password is reported instead of the form
		   silently staying up (bug list "wrong credentials are not reported"). */
		loginError_ = new QLabel(loginPage_);
		loginError_->setWordWrap(true);
		loginError_->setStyleSheet("color:#e58a8a;padding:0 8px;");
		loginError_->hide();
		lp->addWidget(loginError_);
		/* The username/password rows (labels + inputs) + login button live in
		   their own widget so the HTTPS notice can hide the entire form at once
		   without leaving the column labels behind. */
		loginFields_ = new QWidget(loginPage_);
		auto *lf = new QVBoxLayout(loginFields_);
		lf->setContentsMargins(0, 0, 0, 0);
		auto *uRow = new QHBoxLayout;
		uRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.AuthUsername"),
					  loginFields_));
		loginUser_ = new QLineEdit(loginFields_);
		uRow->addWidget(loginUser_, 1);
		lf->addLayout(uRow);
		auto *pRow = new QHBoxLayout;
		pRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.AuthPassword"),
					  loginFields_));
		loginPass_ = new QLineEdit(loginFields_);
		loginPass_->setEchoMode(QLineEdit::Password);
		pRow->addWidget(loginPass_, 1);
		lf->addLayout(pRow);
		loginBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.AuthLogin"), loginFields_);
		lf->addWidget(loginBtn_, 0, Qt::AlignRight);
		lp->addWidget(loginFields_);
		connect(loginBtn_, &QPushButton::clicked, this, [this]() {
			emit loginRequested(loginUser_->text(), loginPass_->text());
		});
	}
	loginPage_->hide();
	layout->addWidget(loginPage_);

	subtabList_ = new QListWidget(this);
	subtabList_->setMaximumWidth(150);
	populateGroups();

	rowsContainer_ = new QWidget(this);
	auto *rowLayout = new QVBoxLayout(rowsContainer_);
	rowLayout->setAlignment(Qt::AlignTop);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setWidget(rowsContainer_);

	auto *h = new QHBoxLayout;
	h->addWidget(subtabList_);
	h->addWidget(scroll, 1);
	layout->addLayout(h, 1);

	connect(subtabList_, &QListWidget::currentTextChanged, this,
		&ZcSettingsPanel::onGroupSelected);
	connect(refresh, &QPushButton::clicked, this,
		&ZcSettingsPanel::refreshGroup);

	/* Default to the first group so its rows are visible immediately rather
	   than leaving the rows pane blank until the user clicks a group. This
	   runs last: it emits currentTextChanged, so the rows pane and the
	   connection above must already exist or the group would never load. */
	if (subtabList_->count() > 0)
		subtabList_->setCurrentRow(0);
}

/* The camera answered 401: show the in-page login form and hide the settings
   rows until a login is entered and accepted. */
void ZcSettingsPanel::showLoginPrompt(const QString &host)
{
	loginHost_ = host;
	if (loginError_)
		loginError_->hide();
	promptHint_->setText(ztr("ZCameraPlugin.Dock.AuthNeeded").arg(host));
	loginFields_->show();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
	loginUser_->setFocus();
}

/* The camera rejected the submitted credentials: show the form again with a
   notice so the operator knows the login was tried and failed rather than the
   click doing nothing (bug list "wrong credentials are not reported"). */
void ZcSettingsPanel::showLoginError()
{
	if (loginError_) {
		loginError_->setText(ztr("ZCameraPlugin.Dock.AuthFailed"));
		loginError_->show();
	}
	loginFields_->show();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
	loginPass_->clear();
	loginPass_->setFocus();
}

/* HTTPS-only camera with no TLS backend: tell the operator to switch to HTTP,
   in the pane (no modal dialog). */
void ZcSettingsPanel::showTlsPrompt(const QString &host)
{
	loginHost_ = host;
	promptHint_->setText(ztr("ZCameraPlugin.Dock.TlsHttpHint").arg(host));
	loginFields_->hide();
	loginPage_->show();
	subtabList_->hide();
	rowsContainer_->hide();
}

void ZcSettingsPanel::hidePrompts()
{
	if (loginPage_)
		loginPage_->hide();
	subtabList_->show();
	rowsContainer_->show();
}

void ZcSettingsPanel::setClient(ZcCameraClient *client)
{
	client_ = client;
	/* Another camera's document: the bar must not be painted from it while the
	   new one is being read, and the index has to be discovered again because
	   the two cameras need not run the same firmware. */
	streamIndex_.clear();
	streamDoc_ = QJsonObject();
	updateStreamBar();
	if (client_) {
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			&ZcSettingsPanel::onGroupRefreshed);
		/* The /info reply is what says whether this camera has a pan/tilt
		   head, and therefore whether it has PTZ settings at all. */
		connect(client_, &ZcCameraClient::cameraInfoChanged, this, [this]() {
			populateGroups();
			buildRows(catalog_);
			if (!client_)
				return;
			/* The stream bar is always visible, so it is always re-read. */
			loadStream();
			if (!catalog_.isEmpty())
				client_->readGroup(catalog_, nullptr);
		});
	}
	/* The camera changed: rebuild the group list for it (a camera without a
	   pan/tilt head has no PTZ group), repaint from the (empty) cache, then
	   load. */
	populateGroups();
	buildRows(catalog_);
}

void ZcSettingsPanel::refreshGroup()
{
	if (!client_)
		return;
	loadStream();
	if (!catalog_.isEmpty())
		client_->readGroup(catalog_, [](bool) {});
}

void ZcSettingsPanel::writeKey(const QString &key, const QString &value)
{
	if (!client_)
		return;
	client_->writeSetting(key, value);
	/* Re-read only the key that changed, never the whole PTZ catalog: a catalog
	   read comes back through groupRefreshed and rebuilds every row, which tears
	   down the control the operator is still using. The row already shows the new
	   value; the cache is all that needs to follow the camera. */
	if (client_->settingGroupKeys(QLatin1String("ptz")).contains(key))
		client_->readSetting(key, [](const ZcSettingValue &) {});
	/* HTTPS and identity auth restart the camera's web service, and a
	   network-type change can move it to another address: the reply never
	   arrives and the connection drops. The dock is told to wait for the camera
	   and reconnect instead of leaving the pane dead (bug 8). */
	if (zc::settingRebootsCamera(key))
		emit cameraRestarting();
}

/* Read the stream document and repaint the bar. The first read of a camera also
   discovers which index its firmware offers: `app_stream1` when it answers with
   a document, `stream1` otherwise (spec §2 Stream). The index then stays fixed
   for as long as that camera is selected. Every reply is guarded on the client
   and the index it was asked for, so a slow reply can never paint the bar from
   another camera's — or another index's — numbers. */
void ZcSettingsPanel::loadStream()
{
	if (!client_ || !client_->isConnected())
		return;
	ZcCameraClient *c = client_;

	if (streamIndex_.isEmpty()) {
		c->streamSettingQuery(
		    zc::kAppStreamIndex, [this, c](const QJsonObject &doc) {
			    if (c != client_)
				    return;
			    if (zc::isStreamDocument(doc)) {
				    streamIndex_ = zc::kAppStreamIndex;
				    streamDoc_ = doc;
				    updateStreamBar();
				    return;
			    }
			    /* Older firmware: that index does not exist, so read the one
			       it does have. */
			    streamIndex_ = zc::kLegacyStreamIndex;
			    loadStream();
		    });
		return;
	}

	const QString index = streamIndex_;
	c->streamSettingQuery(index, [this, c, index](const QJsonObject &doc) {
		if (c != client_ || index != streamIndex_ ||
		    !zc::isStreamDocument(doc))
			return;
		streamDoc_ = doc;
		updateStreamBar();
	});
}

void ZcSettingsPanel::writeStreamParams(
    const QList<QPair<QString, QString>> &params)
{
	if (!client_ || streamIndex_.isEmpty())
		return;
	ZcCameraClient *c = client_;
	const QString index = streamIndex_;
	c->streamSettingWrite(index, params,
			      [this, c, index](const QJsonObject &doc) {
		/* Repaint from the camera's own answer: a field it refuses (an
		   out-of-range bitrate, a value outside its list) then snaps back
		   instead of showing a value the camera never took. */
		if (c != client_ || index != streamIndex_ ||
		    !zc::isStreamDocument(doc))
			return;
		streamDoc_ = doc;
		updateStreamBar();
	});
}

/* Rebuild the group list from the schema. The PTZ group is camera-side PTZ
   configuration, so a camera without a pan/tilt head has none and must not be
   offered it (spec §2); the group appears as soon as /info says it has one.
   Safe to call before the rows pane exists. */
void ZcSettingsPanel::populateGroups()
{
	const QString keep = catalog_;
	const bool ptz = client_ && client_->cameraSupportsPtz();
	/* Model-specific capability: an E2_F6_Pro or Avatar has no identity-auth or
	   HTTPS setting, so the security group is not offered there. Turning one on
	   through the plugin blanks every setting and cannot be undone from either
	   side (bug 7). An empty model ("not read yet") keeps the group, and the
	   list is rebuilt when /info arrives. */
	const QString model = client_ ? client_->cameraModel() : QString();
	/* The rebuild must not repaint the rows pane through onGroupSelected: the
	   caller decides what to render next. */
	QSignalBlocker blocker(subtabList_);
	subtabList_->clear();
	for (const auto &cat : zc::catalogs()) {
		if (zc::catalogRequiresPtz(cat.id) && !ptz)
			continue;
		if (!zc::catalogSupportedByModel(cat.id, model))
			continue;
		auto *item = new QListWidgetItem(catalogLabel(cat.id), subtabList_);
		item->setData(Qt::UserRole, cat.id);
	}
	/* Keep the selection when that group still exists, else fall back to the
	   first one, so a rebuild does not leave the pane showing a group the list
	   no longer offers. */
	int row = -1;
	for (int i = 0; i < subtabList_->count(); ++i) {
		if (subtabList_->item(i)->data(Qt::UserRole).toString() == keep) {
			row = i;
			break;
		}
	}
	if (row < 0 && subtabList_->count() > 0)
		row = 0;
	if (row >= 0)
		subtabList_->setCurrentRow(row);
	catalog_ = row >= 0 ? subtabList_->item(row)->data(Qt::UserRole).toString()
			    : QString();
}

void ZcSettingsPanel::selectGroup(const QString &catalog)
{
	for (int i = 0; i < subtabList_->count(); ++i) {
		if (subtabList_->item(i)->data(Qt::UserRole).toString() ==
		    catalog) {
			subtabList_->setCurrentRow(i);
			return;
		}
	}
}

void ZcSettingsPanel::onGroupSelected(const QString &catalog)
{
	if (catalog.isEmpty())
		return;
	catalog_ = catalog;
	buildRows(catalog);
	/* Rows are built from the cached definitions; the reply that follows
	   arrives as groupRefreshed and rebuilds them with the current values. */
	if (client_)
		client_->readGroup(catalog, nullptr);
}

void ZcSettingsPanel::onGroupRefreshed(const QString &catalog)
{
	/* A late reply for a group the user already left must not repaint the
	   rows pane. */
	if (catalog == catalog_)
		buildRows(catalog);
}

/* Removes every item from a rows layout, including the widgets owned by the
   nested per-row layouts: deleting the nested layout alone would leave its
   widgets parented to the container and still visible. */
static void clearRows(QLayout *layout)
{
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (QWidget *w = item->widget())
			delete w;
		else if (QLayout *l = item->layout())
			clearRows(l);
		delete item;
	}
}

void ZcSettingsPanel::buildRows(const QString &catalog)
{
	/* Clear existing rows: delete every direct child widget and their
	   layouts. */
	QBoxLayout *rowLayout =
		qobject_cast<QBoxLayout *>(rowsContainer_->layout());
	if (!rowLayout)
		return;
	clearRows(rowLayout);
	rowWidgets_.clear();

	auto addHint = [&](const QString &text) {
		auto *hint = new QLabel(text, rowsContainer_);
		hint->setWordWrap(true);
		hint->setStyleSheet("color:#8a9bb2;padding:8px;");
		rowLayout->addWidget(hint);
	};

	if (catalog.isEmpty()) {
		addHint(ztr("ZCameraPlugin.Dock.SelectGroup"));
		return;
	}
	if (!client_) {
		addHint(ztr("ZCameraPlugin.Dock.SelectCamera"));
		return;
	}
	if (!client_->isConnected()) {
		addHint(ztr("ZCameraPlugin.Dock.NotConnected"));
		return;
	}

	/* Render the keys the camera reported for this catalog, in its order.
	   An empty list means the group was not read yet, or the camera does not
	   have this catalog (getbatch answers code -1 with no cfgs). */
	const QStringList keys = client_->settingGroupKeys(catalog);
	if (keys.isEmpty()) {
		addHint(ztr("ZCameraPlugin.Dock.NoSettingsForGroup"));
		return;
	}

	for (const QString &key : keys) {
		/* The WDR setting is not offered by the camera's web UI (it is reported
		   under the raw id `compose_mode`); showing it here only confuses, so it
		   is hidden (bug list "remove WDR setting"). Likewise the raw `ev` row
		   duplicates `ev_choice` (both set EV, and editing one does not update the
		   other); only `ev_choice` is shown (bug list "keep EV Choice"). The image
		   group also reports `ois_mode` and `vignette`, which have no counterpart
		   on the camera or its web UI (they only ever come back read-only), so
		   they are hidden too (bug list "ois_mode/vignette are not on the
		   camera"). `raw_over_hdmi` is removed from the UI on purpose: RAW has
		   nothing to do with live streaming, and toggling it makes the camera
		   restart its web service, which breaks the stream/audio in the plugin
		   (bug list "remove RAW over HDMI", RD feedback). `ptz_limit` is the
		   Range Limit status, which the PTZ tab's own Range Limit group owns
		   (checkbox + the four edge sliders); rendering it here as a second,
		   independent combo duplicated the control and let the two disagree
		   (bug list "the range limit status appears twice"). */
		if (key == QLatin1String("compose_mode") ||
		    key == QLatin1String("ev") ||
		    key == QLatin1String("ois_mode") ||
		    key == QLatin1String("vignette") ||
		    key == QLatin1String("raw_over_hdmi") ||
		    key == QLatin1String("ptz_limit"))
			continue;
		ZcSettingValue def;
		if (!client_->settingDefinition(key, &def))
			continue;
		/* Unsupported (code -1) keys are hidden; read-only (ro=1) keys are shown
		   but disabled (greyed) so the operator can see the value without being
		   able to change it. */
		if (!def.supported)
			continue;
		const bool readOnly = def.readOnly ||
				      /* A time-lapse interval only matters when the camera
					 is actually time-lapsing; in the normal record mode the
					 field is meaningless and must be greyed (bug list
					 "Video_tl_interval grey when record_mode = Normal"). */
				      (key == QLatin1String("video_tl_interval") &&
				       client_->settingValue(QLatin1String("record_mode")) ==
					       QLatin1String("Normal"));

		auto *row = new QHBoxLayout;
		/* `ptz_common_time` carries milliseconds on the wire but the camera's own
		   web UI shows seconds; display it in seconds and convert on write, and
		   make the unit explicit (bug list "common time unit differs from the web
		   and no unit is shown"). */
		const bool timeInSeconds = key == QLatin1String("ptz_common_time");
		const double scale = timeInSeconds ? 1000.0 : 1.0;
		auto *label = new QLabel(
		    timeInSeconds ? settingLabel(key) + QStringLiteral(" (s)")
				  : settingLabel(key),
		    rowsContainer_);
		label->setMinimumWidth(170);
		label->setStyleSheet("color:#eef2ed;");
		/* The camera's own key stays reachable: a locale may rename a row, but
		   the raw id is what the camera and its web UI use. */
		label->setToolTip(key);
		row->addWidget(label);
		row->addStretch(1);

		QWidget *control = nullptr;
		if (def.type == 1 && !def.choices.isEmpty()) {
			/* Choice -> combo box; set the value before connecting so the
			   initial fill does not fire a spurious write. */
			auto *combo = new QComboBox(rowsContainer_);
			combo->addItems(def.choices);
			combo->setCurrentText(def.value);
			connect(combo, &QComboBox::currentTextChanged, this,
				[this, key](const QString &text) {
					writeKey(key, text);
				});
			/* Changing the unit must reveal the matching speed/time row right
			   away instead of waiting for a refresh (bug list "unit switch does
			   not show the matching speed/time row"). */
			if (key == QLatin1String("ptz_common_speed_unit"))
				connect(combo, &QComboBox::currentTextChanged, this,
					[this](const QString &) {
						updatePtzCommonRows();
					});
			control = combo;
		} else if (def.type == 2 && def.max > def.min && def.step > 0 &&
			   def.step == std::floor(def.step)) {
			/* Range -> slider + live gold value label. */
			auto *wrap = new QWidget(rowsContainer_);
			auto *wl = new QHBoxLayout(wrap);
			wl->setContentsMargins(0, 0, 0, 0);
			auto *slider = new QSlider(Qt::Horizontal, wrap);
			slider->setMinimum(static_cast<int>(def.min / scale));
			slider->setMaximum(static_cast<int>(def.max / scale));
			slider->setSingleStep(
			    qMax(1, static_cast<int>(def.step / scale)));
			slider->setValue(static_cast<int>(def.value.toInt() / scale));
			auto *val = new QLabel(QString::number(slider->value()), wrap);
			val->setMinimumWidth(48);
			val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			val->setStyleSheet("color:#d8b878;");
			connect(slider, &QSlider::valueChanged, val,
				[val](int v) { val->setText(QString::number(v)); });
			connect(slider, &QSlider::sliderReleased, this,
				[this, key, slider, scale] {
					writeKey(key, QString::number(
							  static_cast<qint64>(
							      slider->value() * scale)));
				});
			wl->addWidget(slider, 1);
			wl->addWidget(val);
			control = wrap;
		} else {
			/* Text (type 3), and any key whose metadata cannot drive a
			   combo or slider. */
			auto *edit = new QLineEdit(def.value, rowsContainer_);
			connect(edit, &QLineEdit::editingFinished, this,
				[this, key, edit] {
					writeKey(key, edit->text());
				});
			control = edit;
		}

		if (readOnly)
			control->setEnabled(false);

		control->setMinimumWidth(200);
		control->setMaximumWidth(260);
		row->addWidget(control);
		rowLayout->addLayout(row);
		rowWidgets_.insert(key, {label, control});
	}

	/* Apply the PTZ common-speed-unit linkage to the rows just built. */
	updatePtzCommonRows();
}

/* The PTZ group's "common speed unit" selects which of the two common values
   (speed index or recall time) applies. The camera's web UI shows only the one
   in use; until now both stayed visible here (bug list "switching the common
   speed unit does not show the matching speed/time row"). */
void ZcSettingsPanel::updatePtzCommonRows()
{
	const auto unitRow =
	    rowWidgets_.constFind(QLatin1String("ptz_common_speed_unit"));
	if (unitRow == rowWidgets_.constEnd())
		return; /* not the PTZ group */

	/* Prefer the combo on screen (a click has not been round-tripped through the
	   camera yet); fall back to the cached value. unit 1 = Time. */
	QString unitText;
	if (unitRow.value().size() > 1)
		if (auto *combo = qobject_cast<QComboBox *>(unitRow.value().at(1)))
			unitText = combo->currentText();
	if (unitText.isEmpty() && client_)
		unitText = client_->settingValue(
		    QLatin1String("ptz_common_speed_unit"));

	const bool timeMode =
	    unitText == QLatin1String("1") ||
	    unitText.contains(QLatin1String("time"), Qt::CaseInsensitive);

	auto show = [this](const char *key, bool visible) {
		for (QWidget *w : rowWidgets_.value(QLatin1String(key)))
			w->setVisible(visible);
	};
	show("ptz_common_speed", !timeMode);
	show("ptz_common_time", timeMode);
}

/* A JSON array of choices as a string list; the lists are numbers for fps and
   strings for the encoders and resolutions. */
static QStringList jsonStringList(const QJsonValue &value)
{
	QStringList out;
	for (const QJsonValue &entry : value.toArray())
		out << (entry.isString() ? entry.toString()
					 : QString::number(entry.toInt()));
	return out;
}

/* Fill one choice box from the camera's own list. A current value the list does
   not mention is added, so the box can never misrepresent the camera; a box the
   camera offers no list for (older firmware) or marks read-only is shown but not
   selectable, because that value is the camera's own setting. */
static void fillChoice(QComboBox *box, const QStringList &choices,
		       const QString &current, bool readOnly)
{
	box->clear();
	if (choices.isEmpty()) {
		if (!current.isEmpty())
			box->addItem(current);
		box->setEnabled(false);
		return;
	}
	QStringList items = choices;
	if (!current.isEmpty() && !items.contains(current))
		items.prepend(current);
	box->addItems(items);
	box->setCurrentText(current);
	box->setEnabled(!readOnly);
}

/* The stream bar, at the top of the Settings tab (spec §2).

   It is not a settings group: the encoder's document is not a getbatch catalog,
   and how the camera encodes the network stream is the first thing an operator
   changes, so it must be visible without navigating to a group. The controls are
   created once and only ever updated in place — a write's reply arrives while
   the operator may still have a box open, and rebuilding the row would close
   it. */
void ZcSettingsPanel::buildStreamBar(QBoxLayout *outer)
{
	auto *frame = new QFrame(this);
	frame->setObjectName("StreamBar");
	frame->setStyleSheet(
	    "#StreamBar { background:#1b2422; border:1px solid #3c514e;"
	    " border-radius:4px; }"
	    "#StreamBar QLabel { color:#8a9bb2; font-size:11px; }"
	    "#StreamBar QComboBox { color:#eef2ed; background:#2b3a38;"
	    " border:1px solid #3c514e; border-radius:3px; padding:1px 4px;"
	    " font-size:11px; }"
	    "#StreamBar QComboBox:disabled { color:#6b7a90; }"
	    "#StreamBar QComboBox QAbstractItemView { color:#eef2ed;"
	    " background:#2b3a38; selection-background-color:#d8b878;"
	    " selection-color:#1f2927; }");
	streamBar_ = frame;

	auto *bar = new QVBoxLayout(frame);
	bar->setContentsMargins(8, 6, 8, 6);
	bar->setSpacing(4);

	/* Row 1: what the stream is encoded as, plus the index it belongs to. */
	auto *row1 = new QHBoxLayout;
	row1->setSpacing(6);
	auto *title = new QLabel(ztr("ZCameraPlugin.Dock.Stream"), frame);
	title->setStyleSheet("color:#8a9bb2;font-size:11px;letter-spacing:1px;");
	row1->addWidget(title);

	auto addCombo = [&](const QString &label, QComboBox **box) {
		row1->addWidget(new QLabel(label, frame));
		auto *combo = new QComboBox(frame);
		combo->setMinimumWidth(92);
		row1->addWidget(combo);
		*box = combo;
	};
	addCombo(ztr("ZCameraPlugin.Dock.Codec"), &streamCodec_);
	addCombo(ztr("ZCameraPlugin.Dock.Resolution"), &streamRes_);
	addCombo(ztr("ZCameraPlugin.Dock.Fps"), &streamFps_);
	row1->addStretch(1);
	streamInfo_ = new QLabel(frame);
	streamInfo_->setStyleSheet("color:#8a9bb2;font-size:11px;");
	row1->addWidget(streamInfo_);
	bar->addLayout(row1);

	/* Row 2: the two numbers, each with its live gold value. */
	auto *row2 = new QHBoxLayout;
	row2->setSpacing(6);
	auto addSlider = [&](const QString &label, QSlider **slider, QLabel **value) {
		row2->addWidget(new QLabel(label, frame));
		auto *s = new QSlider(Qt::Horizontal, frame);
		s->setMinimumWidth(140);
		s->setMaximumWidth(220);
		row2->addWidget(s);
		auto *v = new QLabel(frame);
		v->setMinimumWidth(60);
		v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		v->setStyleSheet("color:#d8b878;font-size:11px;");
		row2->addWidget(v);
		*slider = s;
		*value = v;
	};
	addSlider(ztr("ZCameraPlugin.Dock.Bitrate"), &streamBitrate_, &streamBitrateValue_);
	addSlider(ztr("ZCameraPlugin.Dock.Gop"), &streamGop_, &streamGopValue_);
	row2->addStretch(1);
	bar->addLayout(row2);

	/* Writes go out on release (a drag must not send a request per pixel) and
	   the bar then repaints from the camera's own answer. */
	connect(streamBitrate_, &QSlider::valueChanged, this, [this](int v) {
		streamBitrateValue_->setText(ztr("ZCameraPlugin.Dock.Kbps").arg(v));
	});
	connect(streamBitrate_, &QSlider::sliderReleased, this, [this]() {
		writeStreamParams({{QStringLiteral("bitrate"),
				    QString::number(streamBitrate_->value() *
						    zc::kBitrateWriteScale)}});
	});
	connect(streamGop_, &QSlider::valueChanged, this, [this](int v) {
		streamGopValue_->setText(QString::number(v));
	});
	connect(streamGop_, &QSlider::sliderReleased, this, [this]() {
		writeStreamParams({{QStringLiteral("gop_n"),
				    QString::number(streamGop_->value())}});
	});
	/* The write names are the firmware's, not the document's field names
	   (spec §2 Stream): `venc` for the encoder, `width`+`height` for a
	   resolution. */
	connect(streamCodec_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams({{QStringLiteral("venc"), text}});
		});
	connect(streamRes_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams(zc::resolutionParams(text));
		});
	connect(streamFps_, &QComboBox::currentTextChanged, this,
		[this](const QString &text) {
			writeStreamParams({{QStringLiteral("fps"), text}});
		});

	outer->addWidget(frame);
	updateStreamBar();
}

/* Repaint the bar from the last document. Never a write: every control is
   filled with its signals blocked, so the operator's own change is the only
   thing that reaches the camera. */
void ZcSettingsPanel::updateStreamBar()
{
	if (!streamBar_)
		return;
	QSignalBlocker blockCodec(streamCodec_);
	QSignalBlocker blockRes(streamRes_);
	QSignalBlocker blockFps(streamFps_);
	QSignalBlocker blockBitrate(streamBitrate_);
	QSignalBlocker blockGop(streamGop_);

	if (streamDoc_.isEmpty()) {
		/* Nothing read yet: leave the bar inert rather than showing values no
		   camera reported. */
		streamCodec_->clear();
		streamRes_->clear();
		streamFps_->clear();
		streamBitrate_->setRange(0, 0);
		streamGop_->setRange(0, 0);
		streamBitrateValue_->setText(QStringLiteral("—"));
		streamGopValue_->setText(QStringLiteral("—"));
		streamInfo_->setText(client_ && client_->isConnected() ? ztr("ZCameraPlugin.Dock.Reading")
								      : ztr("ZCameraPlugin.Dock.NoCamera"));
		streamBar_->setEnabled(false);
		return;
	}

	streamBar_->setEnabled(true);

	/* bitrate is kbps in the document and bps on the wire, and maxBitrate is not
	   a hard bound (an idle stream can report more than it), so the current
	   value is always kept representable. The minimum is 1, not 0: a zero
	   target bitrate is refused by the camera, and the bar then snapped back to
	   the previous value, which read as "the slider cannot reach its left end"
	   (bug list "bitrate slider bounces at the far left"). */
	const int bitrate = streamDoc_.value("bitrate").toInt();
	streamBitrate_->setRange(1, qMax(streamDoc_.value("maxBitrate").toInt(),
					 qMax(1, bitrate)));
	streamBitrate_->setValue(bitrate);
	streamBitrate_->setEnabled(!streamDoc_.value("bitrateRo").toBool());
	streamBitrateValue_->setText(ztr("ZCameraPlugin.Dock.Kbps").arg(bitrate));

	const int gop = streamDoc_.value("gop_n").toInt();
	streamGop_->setRange(1, qMax(streamDoc_.value("maxGop").toInt(), gop));
	streamGop_->setValue(gop);
	streamGop_->setEnabled(!streamDoc_.value("gopRo").toBool());
	streamGopValue_->setText(QString::number(gop));

	const QString resolution =
	    QStringLiteral("%1x%2")
		.arg(streamDoc_.value("width").toInt())
		.arg(streamDoc_.value("height").toInt());
	fillChoice(streamRes_, jsonStringList(streamDoc_.value("resolutionList")),
		   resolution, streamDoc_.value("resolutionRo").toBool());
	fillChoice(streamFps_, jsonStringList(streamDoc_.value("fpsList")),
		   QString::number(streamDoc_.value("fps").toInt()),
		   streamDoc_.value("fpsRo").toBool());
	/* The H.265 10-bit encoder is offered by newer firmware but selecting it
	   does not take effect on the camera side (bug list "H265_10bit cannot be
	   selected"); it is blocked here until the camera accepts it. The camera's
	   own label for it is not fixed, so match on the 10-bit marker. */
	QStringList encoders = jsonStringList(streamDoc_.value("videoEncoderList"));
	encoders.erase(std::remove_if(encoders.begin(), encoders.end(),
				      [](const QString &e) {
					      const QString low = e.toLower();
					      return low.contains(QStringLiteral("10bit")) ||
						     low.contains(QStringLiteral("10-bit")) ||
						     low.contains(QStringLiteral("10_bit"));
				      }),
		       encoders.end());
	fillChoice(streamCodec_, encoders,
		   streamDoc_.value("encoderType").toString(),
		   streamDoc_.value("encoderRo").toBool());

	/* Which index the firmware offered, how deep the stream is and whether it is
	   running: read-only, and the context for everything above. */
	QStringList info;
	info << streamIndex_;
	const QString bitwidth = streamDoc_.value("bitwidth").toString();
	if (!bitwidth.isEmpty())
		info << bitwidth;
	const QString status = streamDoc_.value("status").toString();
	if (!status.isEmpty())
		info << status;
	streamInfo_->setText(info.join(QStringLiteral(" · ")));
}

/* ---------------------------------------------------------------- */
/* ZcPtzPad                                                          */
/* ---------------------------------------------------------------- */

namespace {
constexpr qreal kPi = 3.1415926535897932384626433832795;

/* Per-sector (index 0..7) pan/tilt unit deltas; positive tilt = down,
   positive pan = right (matches ZcCameraClient::ptzMove). */
constexpr int kPanDelta[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kTiltDelta[8] = {0, 1, 1, 1, 0, -1, -1, -1};

/* The camera's /ctrl/pt takes a direction action, not pan/tilt numbers. Map a
   pan/tilt pair (each -1/0/1; +tilt = down, +pan = right) to that action. */
static const char *ptzActionFor(int pan, int tilt)
{
	static const char *const kActions[3][3] = {
		{"leftup", "up", "rightup"},       /* tilt -1 (up)   */
		{"left", "stop", "right"},         /* tilt 0         */
		{"leftdown", "down", "rightdown"}, /* tilt +1 (down) */
	};
	const int p = (pan > 0) - (pan < 0);
	const int t = (tilt > 0) - (tilt < 0);
	return kActions[t + 1][p + 1];
}

const QString kPtzPanelStyle =
    "QWidget#PtzPadWrapper { background:#1f2927; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:6px 10px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QSlider::groove:horizontal { height:4px; background:#3c514e; "
    "border-radius:2px; }"
    "QSlider::handle:horizontal { width:14px; height:14px; margin:-5px 0; "
    "border-radius:7px; background:#d8b878; }";
}

ZcPtzPad::ZcPtzPad(QWidget *parent) : QWidget(parent)
{
	setObjectName("PtzPadWrapper");
	setStyleSheet(kPtzPanelStyle);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	auto *title = new QLabel("PTZ", this);
	layout->addWidget(title, 0, Qt::AlignHCenter);

	/* The painted joystick occupies this fixed, transparent area. */
	joystickArea_ = new QWidget(this);
	joystickArea_->setFixedSize(220, 220);
	joystickArea_->setStyleSheet("background:transparent;");
	joystickArea_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	layout->addWidget(joystickArea_, 0, Qt::AlignHCenter);

	auto *speedRow = new QHBoxLayout;
	auto *speedLbl = new QLabel(ztr("ZCameraPlugin.Dock.Speed"), this);
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setObjectName("ptzSpeed");
	speedSlider_->setRange(1, 100);
	speedSlider_->setValue(50);
	speedRow->addWidget(speedLbl);
	speedRow->addWidget(speedSlider_, 1);
	layout->addLayout(speedRow);

	/* Home / Stop: the two camera-wide moves. Presets live in the adjacent
	   ZcPresetPanel, not here. */
	auto *actionRow = new QHBoxLayout;
	homeButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Home"), this);
	stopButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Stop"), this);
	actionRow->addWidget(homeButton_);
	actionRow->addWidget(stopButton_);
	layout->addLayout(actionRow);

	connect(homeButton_, &QPushButton::clicked, this,
		[this]() { emit homeRequested(); });
	connect(stopButton_, &QPushButton::clicked, this, [this]() {
		if (client_)
			client_->ptzStop();
	});

	/* No position / GOTO row here: the absolute / relative GOTO spins are not
	   wanted on this panel — the joystick, the presets and the traces already
	   cover every move it needs. (The client's ptzGoto() stays: it is part of
	   the /ctrl/pt protocol facade.) A read-only Current Position read-out is
	   kept below, though: the operator needs to see where the head is. */

	/* Current Position (bug list: the PTZ tab had no position read-out). Kept
	   live by the ptzfChange notification, so it follows the joystick. */
	positionLabel_ = new QLabel(this);
	positionLabel_->setObjectName("PtzPosition");
	layout->addWidget(positionLabel_);
	setPosition(0.0, 0.0);

	/* No auto-framing controls here: they live in the dock's dedicated
	   ZcFramingPanel tab, matching the camera's web UI layout. */

	/* Range Limit: the enable flag plus the four edge sliders and their
	   "set to current" buttons, mirroring the camera's web UI
	   "Pan tilt -> Range Limit" group (bug list: the four range items were
	   missing). Up/Right write the "upright" corner, Down/Left "downleft",
	   exactly as the web UI does. */
	limitCheck_ = new QCheckBox(ztr("ZCameraPlugin.Dock.PtzRangeLimit"), this);
	layout->addWidget(limitCheck_);

	static const char *const kEdgeLabels[EdgeCount] = {
		"ZCameraPlugin.Dock.EdgeUp", "ZCameraPlugin.Dock.EdgeRight",
		"ZCameraPlugin.Dock.EdgeDown", "ZCameraPlugin.Dock.EdgeLeft"};
	for (int e = 0; e < EdgeCount; ++e) {
		auto *row = new QHBoxLayout;
		row->setSpacing(4);
		auto *lbl = new QLabel(ztr(kEdgeLabels[e]), this);
		lbl->setFixedWidth(36);
		limitSlider_[e] = new QSlider(Qt::Horizontal, this);
		limitSlider_[e]->setRange(0, 1);
		limitValue_[e] = new QLabel(QStringLiteral("0\u00b0"), this);
		limitValue_[e]->setFixedWidth(36);
		limitValue_[e]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		limitCurrent_[e] = new QPushButton(QStringLiteral("\u2316"), this);
		limitCurrent_[e]->setFixedWidth(28);
		limitCurrent_[e]->setToolTip(ztr("ZCameraPlugin.Dock.PtzSetCurrent"));
		row->addWidget(lbl);
		row->addWidget(limitSlider_[e], 1);
		row->addWidget(limitValue_[e]);
		row->addWidget(limitCurrent_[e]);
		layout->addLayout(row);

		connect(limitSlider_[e], &QSlider::valueChanged, this,
			[this, e](int v) {
				updateLimitLabel((Edge)e);
				if (!limitLoaded_)
					return;
				/* Keep the opposite edge one degree clear of this one,
				   without re-entering this handler. */
				const bool low = (e == EdgeDown || e == EdgeLeft);
				QSlider *opp = (e == EdgeUp)     ? limitSlider_[EdgeDown]
					     : (e == EdgeRight) ? limitSlider_[EdgeLeft]
					     : (e == EdgeDown)  ? limitSlider_[EdgeUp]
								: limitSlider_[EdgeRight];
				{
					QSignalBlocker b(opp);
					if (low)
						opp->setMinimum(v + 1);
					else
						opp->setMaximum(v - 1);
				}
				updateLimitLabel((Edge)((e + 2) % EdgeCount));
			});
		/* Write only when the drag ends: every other slider in the dock does the
		   same, and one request per notch floods the camera's single-threaded
		   httpd and can store intermediate limits (bug list: range-limit
		   sliders). */
		connect(limitSlider_[e], &QSlider::sliderReleased, this,
			[this, e]() {
				if (!limitLoaded_)
					return;
				pushLimit((Edge)e);
			});
		connect(limitCurrent_[e], &QPushButton::clicked, this,
			[this, e]() { setEdgeToCurrent((Edge)e); });
	}

	connect(limitCheck_, &QCheckBox::toggled, this, [this](bool on) {
		/* Repaint the four sliders from the limit / unlimit box right away, the
		   way the camera's web UI does when its Range Limit status changes: with
		   the limit on an edge sits on the stored boundary, with it off on the
		   head's full travel. */
		if (limitLoaded_)
			applyLimitBox(on);
		if (client_)
			client_->writeSetting(QLatin1String("ptz_limit"),
					      on ? QStringLiteral("On")
						 : QStringLiteral("Off"));
	});

	layout->addStretch();
}

void ZcPtzPad::setClient(ZcCameraClient *client)
{
	client_ = client;
	if (client_) {
		/* A change made from the Settings tab (or the camera's own web UI)
		   reaches the engine as a refreshed PTZ catalog. Mirror its `ptz_limit`
		   so the Range Limit checkbox and the four sliders below stay in step
		   with it, instead of the two controls disagreeing (bug list: the range
		   limit status did not drive the range limit adjustment). */
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			[this](const QString &catalog) {
				if (!client_ || catalog != QLatin1String("ptz"))
					return;
				const QString value = client_->settingValue(
				    QLatin1String("ptz_limit"));
				if (value.isEmpty())
					return;
				const bool enabled = value == QLatin1String("On");
				if (limitCheck_) {
					QSignalBlocker b(limitCheck_);
					limitCheck_->setChecked(enabled);
				}
				if (limitLoaded_)
					applyLimitBox(enabled);
			});
		return;
	}
	/* No client: clear the live values so a stale position/limit from the
	   previous camera is not left on screen. */
	limitLoaded_ = false;
	if (limitCheck_) {
		QSignalBlocker b(limitCheck_);
		limitCheck_->setChecked(false);
	}
	for (int e = 0; e < EdgeCount; ++e) {
		if (!limitSlider_[e])
			continue;
		QSignalBlocker b(limitSlider_[e]);
		limitSlider_[e]->setRange(0, 1);
		limitSlider_[e]->setValue(0);
		updateLimitLabel((Edge)e);
	}
	if (positionLabel_)
		positionLabel_->setText(ztr("ZCameraPlugin.Dock.PtzPosition") +
					QStringLiteral(" \u2014"));
}

/* Whole-degree slider helper: a degenerate range (an edge that reaches the
   camera's own travel) collapses to a single point rather than inverting. */
static void setLimitRange(QSlider *s, int lo, int hi, int value)
{
	if (!s)
		return;
	if (hi < lo)
		hi = lo;
	s->setRange(lo, hi);
	s->setValue(value);
}

void ZcPtzPad::applyLimitBox(bool enabled)
{
	/* Ranges and values mirror the web UI's updatePtzLimit(): with the limit on
	   an edge sits on the stored boundary, with it off on the camera's full
	   travel. The opposite edge is kept one degree clear so the box cannot
	   invert. */
	const int upVal = enabled ? tiltMax_ : utiltMax_;
	const int rightVal = enabled ? panMax_ : upanMax_;
	const int downVal = enabled ? tiltMin_ : utiltMin_;
	const int leftVal = enabled ? panMin_ : upanMin_;
	for (int e = 0; e < EdgeCount; ++e) {
		if (limitSlider_[e])
			limitSlider_[e]->blockSignals(true);
	}
	setLimitRange(limitSlider_[EdgeUp], tiltMin_ + 1, utiltMax_, upVal);
	setLimitRange(limitSlider_[EdgeRight], panMin_ + 1, upanMax_, rightVal);
	setLimitRange(limitSlider_[EdgeDown], utiltMin_, tiltMax_ - 1, downVal);
	setLimitRange(limitSlider_[EdgeLeft], upanMin_, panMax_ - 1, leftVal);
	for (int e = 0; e < EdgeCount; ++e) {
		if (limitSlider_[e])
			limitSlider_[e]->blockSignals(false);
		updateLimitLabel((Edge)e);
	}
}

void ZcPtzPad::pushLimit(Edge edge)
{
	if (!client_)
		return;
	const int up = limitSlider_[EdgeUp]->value();
	const int right = limitSlider_[EdgeRight]->value();
	const int down = limitSlider_[EdgeDown]->value();
	const int left = limitSlider_[EdgeLeft]->value();
	/* Up and Right are the origin corner's maximums, Down and Left its
	   minimums; the camera takes the whole corner in one request. */
	if (edge == EdgeUp || edge == EdgeRight)
		client_->ptzLimitUpdate(QStringLiteral("upright"), right, up);
	else
		client_->ptzLimitUpdate(QStringLiteral("downleft"), left, down);
	/* Re-read the limit box the camera now holds: the transport serializes
	   requests, so this reply reflects the push. Without it the cached box
	   goes stale and the next Range-Limit checkbox toggle (applyLimitBox)
	   repaints the sliders back to the pre-drag limits. */
	refreshState();
}

void ZcPtzPad::updateLimitLabel(Edge edge)
{
	if (limitValue_[edge] && limitSlider_[edge])
		limitValue_[edge]->setText(
		    QString::number(limitSlider_[edge]->value()) +
		    QStringLiteral("\u00b0"));
}

void ZcPtzPad::setEdgeToCurrent(Edge edge)
{
	if (!client_)
		return;
	client_->ptzQuery(true, [this, edge](const QJsonObject &o) {
		if (!client_ || !limitLoaded_ || !limitSlider_[edge])
			return;
		const int pan = qRound(o.value("pan_pos").toDouble());
		const int tilt = qRound(o.value("tilt_pos").toDouble());
		{
			QSignalBlocker b(limitSlider_[edge]);
			limitSlider_[edge]->setValue(
			    (edge == EdgeUp || edge == EdgeDown) ? tilt : pan);
		}
		updateLimitLabel(edge);
		if (limitCheck_ && !limitCheck_->isChecked()) {
			QSignalBlocker b(limitCheck_);
			limitCheck_->setChecked(true);
		}
		pushLimit(edge);
	});
}

void ZcPtzPad::refreshState()
{
	if (!client_)
		return;
	client_->ptzQuery(true, [this](const QJsonObject &o) {
		if (!client_)
			return;
		if (o.contains("pan_pos") || o.contains("tilt_pos"))
			setPosition(o.value("pan_pos").toDouble(),
				    o.value("tilt_pos").toDouble());
		const QJsonObject lim = o.value("limit").toObject();
		const QJsonObject un = o.value("unlimit").toObject();
		if (lim.isEmpty() || un.isEmpty())
			return;
		panMin_ = qRound(lim.value("pan_min").toDouble());
		panMax_ = qRound(lim.value("pan_max").toDouble());
		tiltMin_ = qRound(lim.value("tilt_min").toDouble());
		tiltMax_ = qRound(lim.value("tilt_max").toDouble());
		upanMin_ = qRound(un.value("pan_min").toDouble());
		upanMax_ = qRound(un.value("pan_max").toDouble());
		utiltMin_ = qRound(un.value("tilt_min").toDouble());
		utiltMax_ = qRound(un.value("tilt_max").toDouble());
		const bool enabled = lim.value("enable").toBool();
		if (limitCheck_) {
			QSignalBlocker b(limitCheck_);
			limitCheck_->setChecked(enabled);
		}
		limitLoaded_ = true;
		applyLimitBox(enabled);
	});
}

void ZcPtzPad::setPosition(double pan, double tilt)
{
	if (!positionLabel_)
		return;
	positionLabel_->setText(
	    QStringLiteral("%1  Pan %2\u00b0   Tilt %3\u00b0")
		.arg(ztr("ZCameraPlugin.Dock.PtzPosition"),
		     QString::number(pan, 'f', 1),
		     QString::number(tilt, 'f', 1)));
}

void ZcPtzPad::updateGeometry()
{
	joystickRect_ = QRectF(joystickArea_->geometry());
	if (joystickRect_.isEmpty())
		return;
	joyCenter_ = joystickRect_.center();
	joyOuterRadius_ = joystickRect_.width() / 2.0 - 12.0;
	joyInnerRadius_ = joyOuterRadius_ * 0.46;
}

QPainterPath ZcPtzPad::sectorPath(int i) const
{
	const qreal a0 = i * 45.0 - 22.5;
	const qreal a1 = i * 45.0 + 22.5;
	QPainterPath path;
	const int steps = 5; /* ~9° per chord is smooth enough */
	for (int s = 0; s <= steps; ++s) {
		qreal a = a0 + (a1 - a0) * s / steps;
		qreal rad = a * kPi / 180.0;
		QPointF pt(joyCenter_.x() + joyOuterRadius_ * std::cos(rad),
			   joyCenter_.y() + joyOuterRadius_ * std::sin(rad));
		if (s == 0)
			path.moveTo(pt);
		else
			path.lineTo(pt);
	}
	for (int s = steps; s >= 0; --s) {
		qreal a = a0 + (a1 - a0) * s / steps;
		qreal rad = a * kPi / 180.0;
		QPointF pt(joyCenter_.x() + joyInnerRadius_ * std::cos(rad),
			   joyCenter_.y() + joyInnerRadius_ * std::sin(rad));
		path.lineTo(pt);
	}
	path.closeSubpath();
	return path;
}

void ZcPtzPad::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	updateGeometry();

	if (joystickRect_.isEmpty())
		return;

	/* Background ring. */
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x2a, 0x38, 0x36));
	p.drawEllipse(joyCenter_, joyOuterRadius_, joyOuterRadius_);

	/* 8 animated/directional sectors. */
	for (int i = 0; i < 8; ++i) {
		bool active = (mode_ == JoystickMode::Sector && activeDir_ == i);
		p.setBrush(active ? QColor(0xd8, 0xb8, 0x78)
				  : QColor(0x22, 0x2c, 0x2a));
		p.drawPath(sectorPath(i));

		/* Directional arrow pointing outward at the sector's mid angle. */
		qreal mid = i * 45.0;
		qreal rad = mid * kPi / 180.0;
		qreal ar = (joyInnerRadius_ + joyOuterRadius_) / 2.0;
		p.save();
		p.translate(joyCenter_.x() + ar * std::cos(rad),
			    joyCenter_.y() + ar * std::sin(rad));
		p.rotate(mid); /* screen coords: rotate() is clockwise, matches */
		QPainterPath arrow;
		arrow.moveTo(5, 0);
		arrow.lineTo(-3, -3.2);
		arrow.lineTo(-3, 3.2);
		arrow.closeSubpath();
		p.setBrush(active ? QColor(0x1f, 0x29, 0x27)
				  : QColor(0x8a, 0x9b, 0xb2));
		p.drawPath(arrow);
		p.restore();
	}

	/* Inner hole. */
	p.setBrush(QColor(0x18, 0x1e, 0x1c));
	p.drawEllipse(joyCenter_, joyInnerRadius_, joyInnerRadius_);

	/* Center draggable knob. */
	QPointF knob = joyCenter_ + dragOffset_;
	qreal knobR = qMax(6.0, joyInnerRadius_ * 0.42);
	bool dragging = (mode_ == JoystickMode::FreeDrag);
	p.setBrush(dragging ? QColor(0xd8, 0xb8, 0x78)
			    : QColor(0x3c, 0x51, 0x4e));
	p.drawEllipse(knob, knobR, knobR);
	p.setBrush(dragging ? QColor(0x1f, 0x29, 0x27)
			    : QColor(0x8a, 0x9b, 0xb2));
	qreal dotR = knobR * 0.34;
	p.drawEllipse(knob, dotR, dotR);
}

int ZcPtzPad::directionAt(const QPoint &pos) const
{
	QPointF d = QPointF(pos) - joyCenter_;
	qreal dist = std::sqrt(d.x() * d.x() + d.y() * d.y());
	if (dist < joyInnerRadius_ || dist > joyOuterRadius_ + 6.0)
		return -1;
	qreal a = std::atan2(d.y(), d.x()) * 180.0 / kPi; /* [-180,180] */
	int idx = qRound(a / 45.0) % 8;
	if (idx < 0)
		idx += 8;
	return idx;
}

bool ZcPtzPad::inCenter(const QPoint &pos) const
{
	QPointF d = QPointF(pos) - joyCenter_;
	return std::sqrt(d.x() * d.x() + d.y() * d.y()) <= joyInnerRadius_ * 0.6;
}

void ZcPtzPad::moveInDirection(int dirIndex)
{
	if (dirIndex < 0 || dirIndex > 7)
		return;
	const int pan = kPanDelta[dirIndex];
	const int tilt = kTiltDelta[dirIndex];
	/* fspeed is 0-1; the slider is 0-100. */
	const float fspeed = speedSlider_->value() / 100.0f;
	if (client_)
		client_->ptzMoveAction(QLatin1String(ptzActionFor(pan, tilt)), fspeed);
	emit moveRequested(pan, tilt);
}

void ZcPtzPad::moveAnalog(const QPointF &offset)
{
	qreal px = offset.x() / joyOuterRadius_;
	qreal py = offset.y() / joyOuterRadius_;
	px = std::max<qreal>(-1.0, std::min<qreal>(1.0, px));
	py = std::max<qreal>(-1.0, std::min<qreal>(1.0, py));
	/* Drag distance from centre is the speed (0-1); the direction is where the
	   knob is dragged (bug 9). */
	const float fspeed = (float)std::min<qreal>(1.0, std::hypot(px, py));
	const int pan = (px > 0) - (px < 0);
	const int tilt = (py > 0) - (py < 0);
	if (client_)
		client_->ptzMoveAction(QLatin1String(ptzActionFor(pan, tilt)), fspeed);
	emit moveRequested(pan, tilt);
}

void ZcPtzPad::stopMoving()
{
	if (client_)
		client_->ptzStop();
}

void ZcPtzPad::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || mode_ != JoystickMode::None) {
		QWidget::mousePressEvent(event);
		return;
	}
	updateGeometry();
	if (inCenter(event->pos())) {
		mode_ = JoystickMode::FreeDrag;
		dragOffset_ = QPointF(0, 0);
	} else {
		int idx = directionAt(event->pos());
		if (idx >= 0) {
			mode_ = JoystickMode::Sector;
			activeDir_ = idx;
			moveInDirection(idx);
			update();
		}
	}
	if (mode_ != JoystickMode::None) {
		grabMouse();
		event->accept();
	} else {
		QWidget::mousePressEvent(event);
	}
}

void ZcPtzPad::mouseMoveEvent(QMouseEvent *event)
{
	if (mode_ == JoystickMode::None) {
		QWidget::mouseMoveEvent(event);
		return;
	}
	if (mode_ == JoystickMode::Sector) {
		int idx = directionAt(event->pos());
		if (idx >= 0 && idx != activeDir_) {
			activeDir_ = idx;
			moveInDirection(idx);
		} else if (idx < 0) {
			if (activeDir_ >= 0) {
				stopMoving();
				activeDir_ = -1;
			}
		}
		update();
	} else if (mode_ == JoystickMode::FreeDrag) {
		QPointF d = QPointF(event->pos()) - joyCenter_;
		qreal lim = qMax(8.0, joyInnerRadius_ * 0.42);
		qreal dist = std::sqrt(d.x() * d.x() + d.y() * d.y());
		if (dist > lim) {
			d = d * (lim / dist);
		}
		dragOffset_ = d;
		moveAnalog(d);
		update();
	}
	event->accept();
}

void ZcPtzPad::mouseReleaseEvent(QMouseEvent *event)
{
	if (mode_ == JoystickMode::None) {
		QWidget::mouseReleaseEvent(event);
		return;
	}
	if (event->button() == Qt::LeftButton) {
		stopMoving();
		mode_ = JoystickMode::None;
		activeDir_ = -1;
		dragOffset_ = QPointF(0, 0);
		releaseMouse();
		update();
	}
	event->accept();
}

/* ---------------------------------------------------------------- */
/* ZcFramingPanel                                                    */
/* ---------------------------------------------------------------- */

namespace {
/* Re-read the framing state a moment after an apply, once the camera has
   stored it. */
constexpr int kFramingReloadMs = 400;

/* The auto-framing panel speaks the whole /ctrl/framing API the camera's own
   web page uses. Its canvas-only parts (the framing-box drag, the FOV preview,
   EzFraming click-to-centre) are not reproduced, but the switches, dropdowns,
   spinners and composition presets are, so the tab is shown for a camera whose
   /info reports auto framing (bug list: "the whole auto-framing settings are
   missing"). The gate is cameraSupportsAutoFraming() in updatePtzTab(). */
constexpr bool kFramingPanelVisible = true;

/* Option lists lifted from the camera's web UI (framing.js constants
   AI_FRAMING_SCENE / AI_TARET_NUM / AI_TARGET_LOST_ACTION); /ctrl/framing/config
   reports the same lists. */
const char *const kFramingScenes[] = {"General", "Sports"};
const char *const kFramingLostActions[] = {"Hold Position", "Wide",
					   "Start Position"};
constexpr int kFramingTargetNumMin = 1;
constexpr int kFramingTargetNumMax = 8;
/* The camera's speed sliders run the full signed range on this firmware. */
constexpr int kFramingSpeedMin = -100;
constexpr int kFramingSpeedMax = 100;
/* Fallback start-position labels by index, used when all_status carries no
   start_pos_opts (the camera's own list is zero-padded: "Preset 001"…). */
const char *const kFramingStartPosFallback[] = {
    "Home", "Preset 001", "Preset 002", "Preset 003", "Preset 004",
    "Preset 005"};

/* The camera limits preset names by UTF-8 byte count (max_name_len), so a
   Chinese name has fewer characters than an English one. Clamp whole
   characters until it fits, like the web UI does. */
QString framingClampName(const QString &name, int maxBytes)
{
	if (maxBytes <= 0 || name.toUtf8().size() <= maxBytes)
		return name;
	QString out = name;
	while (!out.isEmpty() && out.toUtf8().size() > maxBytes)
		out.chop(1);
	return out;
}

const QString kFramingPanelStyle =
    "QWidget#FramingPanel { background:#1f2927; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QLabel#FramingStatus { color:#d8b878; font-size:11px; letter-spacing:0px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:6px 12px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QPushButton:disabled { color:#5d6b7d; background:#242f2d; }"
    "QCheckBox { color:#eef2ed; font-size:11px; spacing:6px; }"
    "QGroupBox { color:#d8b878; font-size:11px; border:1px solid #33413f; "
    "border-radius:4px; margin-top:8px; padding:8px 6px 6px 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left:8px; padding:0 4px; }"
    "QComboBox, QSpinBox, QLineEdit { color:#eef2ed; background:#2b3a38; "
    "border:1px solid #3c514e; border-radius:4px; padding:3px 6px; "
    "font-size:11px; }"
    "QListWidget { color:#eef2ed; background:#232e2c; "
    "border:1px solid #3c514e; border-radius:4px; font-size:11px; }"
    "QListWidget::item:selected { background:#3c514e; }";
}

/* Auto-framing controls in their own tab, split out of the PTZ joystick to match
   the camera's web UI. Every control here is one of the non-canvas controls on
   that page's "Auto Framing" panel; the writes are the same /ctrl/framing/<sub>
   calls, and the reads come from a single /ctrl/framing/all_status snapshot.
   The page's canvas-only parts (framing-box drag, FOV preview, EzFraming
   click-to-centre) are deliberately absent. */
ZcFramingPanel::ZcFramingPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("FramingPanel");
	setStyleSheet(kFramingPanelStyle);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	/* The panel is taller than the dock, so it scrolls instead of clipping. */
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	auto *body = new QWidget;
	scroll->setWidget(body);
	auto *layout = new QVBoxLayout(body);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	auto *title = new QLabel(ztr("ZCameraPlugin.Framing.Title"), body);
	layout->addWidget(title, 0, Qt::AlignHCenter);

	/* What the camera reports right now, kept in sync on refresh and after an
	   apply. */
	statusLabel_ = new QLabel(ztr("ZCameraPlugin.Framing.Reading"), body);
	statusLabel_->setObjectName("FramingStatus");
	statusLabel_->setWordWrap(true);
	layout->addWidget(statusLabel_);

	/* ---- Tracking: the two live switches and the restart button. ---- */
	auto *trackBox = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupTrack"), body);
	auto *trackLay = new QVBoxLayout(trackBox);
	trackLay->setSpacing(6);
	detectChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.Detect"), trackBox);
	traceChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.Trace"), trackBox);
	trackLay->addWidget(detectChk_);
	trackLay->addWidget(traceChk_);
	restartNowBtn_ =
	    new QPushButton(ztr("ZCameraPlugin.Framing.RestartNow"), trackBox);
	restartNowBtn_->setToolTip(ztr("ZCameraPlugin.Framing.RestartNowTip"));
	{
		auto *row = new QHBoxLayout;
		row->addWidget(restartNowBtn_);
		row->addStretch(1);
		trackLay->addLayout(row);
	}
	layout->addWidget(trackBox);

	/* ---- Framing: which subject to keep, and how fast to move. ---- */
	auto *framingBox = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupFraming"), body);
	auto *framingLay = new QGridLayout(framingBox);
	framingLay->setHorizontalSpacing(8);
	framingLay->setVerticalSpacing(6);
	framingLay->setColumnStretch(1, 1);

	autoTargetChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.AutoTarget"), framingBox);
	zoomLockChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.ZoomLock"), framingBox);
	tiltLockChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.TiltLock"), framingBox);
	framingLay->addWidget(autoTargetChk_, 0, 0, 1, 2);
	framingLay->addWidget(zoomLockChk_, 1, 0);
	framingLay->addWidget(tiltLockChk_, 1, 1);

	framingLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.Sensitivity"), framingBox), 2, 0);
	sensitivitySpin_ = new QSpinBox(framingBox);
	sensitivitySpin_->setRange(kFramingSpeedMin, kFramingSpeedMax);
	framingLay->addWidget(sensitivitySpin_, 2, 1);

	framingLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.Acceleration"), framingBox), 3, 0);
	accelerationSpin_ = new QSpinBox(framingBox);
	accelerationSpin_->setRange(kFramingSpeedMin, kFramingSpeedMax);
	framingLay->addWidget(accelerationSpin_, 3, 1);
	layout->addWidget(framingBox);

	/* ---- Composition: where the subject sits in the frame. ---- */
	auto *sceneBox = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupScene"), body);
	auto *sceneLay = new QGridLayout(sceneBox);
	sceneLay->setHorizontalSpacing(8);
	sceneLay->setVerticalSpacing(6);
	sceneLay->setColumnStretch(1, 1);
	int row = 0;

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.Scene"), sceneBox), row, 0);
	sceneCombo_ = new QComboBox(sceneBox);
	for (const char *s : kFramingScenes)
		sceneCombo_->addItem(QString::fromUtf8(s));
	sceneLay->addWidget(sceneCombo_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.StartPos"), sceneBox), row, 0);
	startPosCombo_ = new QComboBox(sceneBox);
	for (int i = 0;
	     i < int(sizeof(kFramingStartPosFallback) / sizeof(kFramingStartPosFallback[0]));
	     i++)
		startPosCombo_->addItem(QString::fromUtf8(kFramingStartPosFallback[i]), i);
	sceneLay->addWidget(startPosCombo_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.TargetNum"), sceneBox), row, 0);
	targetNumCombo_ = new QComboBox(sceneBox);
	for (int i = kFramingTargetNumMin; i <= kFramingTargetNumMax; i++)
		targetNumCombo_->addItem(QString::number(i), i);
	sceneLay->addWidget(targetNumCombo_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.LostAction"), sceneBox), row, 0);
	lostActionCombo_ = new QComboBox(sceneBox);
	for (const char *s : kFramingLostActions)
		lostActionCombo_->addItem(QString::fromUtf8(s));
	sceneLay->addWidget(lostActionCombo_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.LostTimeout"), sceneBox), row, 0);
	lostTimeoutSpin_ = new QSpinBox(sceneBox);
	lostTimeoutSpin_->setRange(0, 60);
	lostTimeoutSpin_->setSuffix(ztr("ZCameraPlugin.Framing.SecondsSuffix"));
	sceneLay->addWidget(lostTimeoutSpin_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.WaitTime"), sceneBox), row, 0);
	waitTimeSpin_ = new QSpinBox(sceneBox);
	waitTimeSpin_->setRange(0, 60);
	waitTimeSpin_->setSuffix(ztr("ZCameraPlugin.Framing.SecondsSuffix"));
	sceneLay->addWidget(waitTimeSpin_, row++, 1);

	sceneLay->addWidget(new QLabel(ztr("ZCameraPlugin.Framing.Restart"), sceneBox), row, 0);
	restartSpin_ = new QSpinBox(sceneBox);
	restartSpin_->setRange(0, 600);
	restartSpin_->setValue(30);
	restartSpin_->setSuffix(ztr("ZCameraPlugin.Framing.SecondsSuffix"));
	sceneLay->addWidget(restartSpin_, row++, 1);

	maskChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.MaskArea"), sceneBox);
	faceTrackChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.FaceTrack"), sceneBox);
	sceneLay->addWidget(maskChk_, row++, 0, 1, 2);
	sceneLay->addWidget(faceTrackChk_, row++, 0, 1, 2);
	layout->addWidget(sceneBox);

	/* ---- Power-on behaviour, tally and gesture triggers. ---- */
	auto *powerBox = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupPower"), body);
	auto *powerLay = new QVBoxLayout(powerBox);
	powerLay->setSpacing(6);
	autoStartChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.AutoStart"), powerBox);
	tallyChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.Tally"), powerBox);
	trackingRecordChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.TrackingRecord"), powerBox);
	wavingChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.Waving"), powerBox);
	gestureFovChk_ = new QCheckBox(ztr("ZCameraPlugin.Framing.GestureFov"), powerBox);
	powerLay->addWidget(autoStartChk_);
	powerLay->addWidget(tallyChk_);
	powerLay->addWidget(trackingRecordChk_);
	powerLay->addWidget(wavingChk_);
	powerLay->addWidget(gestureFovChk_);
	layout->addWidget(powerBox);

	/* ---- Apply / refresh. ---- */
	auto *actionRow = new QHBoxLayout;
	actionRow->setSpacing(6);
	auto *applyBtn = new QPushButton(ztr("ZCameraPlugin.Framing.Apply"), body);
	auto *refreshBtn = new QPushButton(ztr("ZCameraPlugin.Framing.Refresh"), body);
	actionRow->addWidget(applyBtn);
	actionRow->addWidget(refreshBtn);
	actionRow->addStretch(1);
	layout->addLayout(actionRow);

	/* ---- Composition presets. ---- */
	auto *presetBox = new QGroupBox(ztr("ZCameraPlugin.Framing.GroupPreset"), body);
	auto *presetLay = new QVBoxLayout(presetBox);
	presetLay->setSpacing(6);

	presetList_ = new QListWidget(presetBox);
	presetList_->setFixedHeight(110);
	presetLay->addWidget(presetList_);

	presetHint_ = new QLabel(presetBox);
	presetHint_->setWordWrap(true);
	presetLay->addWidget(presetHint_);

	{
		auto *row1 = new QHBoxLayout;
		row1->setSpacing(6);
		auto *recallBtn = new QPushButton(ztr("ZCameraPlugin.Dock.Recall"), presetBox);
		auto *replaceBtn = new QPushButton(ztr("ZCameraPlugin.Dock.Replace"), presetBox);
		auto *renameBtn = new QPushButton(ztr("ZCameraPlugin.Dock.Rename"), presetBox);
		row1->addWidget(recallBtn);
		row1->addWidget(replaceBtn);
		row1->addWidget(renameBtn);
		presetLay->addLayout(row1);

		auto *row2 = new QHBoxLayout;
		row2->setSpacing(6);
		auto *resetBtn = new QPushButton(ztr("ZCameraPlugin.Framing.PresetReset"), presetBox);
		auto *deleteBtn = new QPushButton(ztr("ZCameraPlugin.Dock.Delete"), presetBox);
		presetAddBtn_ = new QPushButton(ztr("ZCameraPlugin.Framing.PresetAdd"), presetBox);
		row2->addWidget(resetBtn);
		row2->addWidget(deleteBtn);
		row2->addStretch(1);
		row2->addWidget(presetAddBtn_);
		presetLay->addLayout(row2);

		/* The camera re-reads and rewrites its preset table asynchronously, so
		   every mutation is followed by a fresh read. */
		const auto reloadPresetsSoon = [this]() {
			QTimer::singleShot(kFramingReloadMs, this,
					   [this]() { refreshPresets(); });
		};
		const auto selectedRow = [this]() { return presetList_->currentRow(); };
		const auto askConfirm = [this](const QString &text) {
			return QMessageBox::question(
				   this, ztr("ZCameraPlugin.Framing.GroupPreset"), text,
				   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
			       QMessageBox::Yes;
		};

		connect(presetList_, &QListWidget::itemDoubleClicked, this,
			[this, selectedRow](QListWidgetItem *) {
				const int i = selectedRow();
				if (client_ && i >= 0)
					client_->framingPresetRecall(i);
			});
		connect(recallBtn, &QPushButton::clicked, this, [this, selectedRow]() {
			const int i = selectedRow();
			if (client_ && i >= 0)
				client_->framingPresetRecall(i);
		});
		connect(replaceBtn, &QPushButton::clicked, this,
			[this, selectedRow, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0 || i >= presetNames_.size())
					return;
				client_->framingPresetUpdate(i, presetNames_.at(i));
				reloadPresetsSoon();
			});
		connect(renameBtn, &QPushButton::clicked, this,
			[this, selectedRow, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0 || i >= presetNames_.size())
					return;
				bool ok = false;
				const QString name = QInputDialog::getText(
				    this, ztr("ZCameraPlugin.Framing.PresetRenameTitle"),
				    ztr("ZCameraPlugin.Framing.PresetName"), QLineEdit::Normal,
				    presetNames_.at(i), &ok);
				if (!ok)
					return;
				const QString trimmed = name.trimmed();
				if (trimmed.isEmpty())
					return;
				client_->framingPresetRename(
				    i, framingClampName(trimmed, presetMaxNameBytes_));
				reloadPresetsSoon();
			});
		connect(resetBtn, &QPushButton::clicked, this,
			[this, selectedRow, askConfirm, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0)
					return;
				if (!askConfirm(ztr("ZCameraPlugin.Framing.PresetResetConfirm")
						    .arg(presetNames_.value(i))))
					return;
				client_->framingPresetReset(i);
				reloadPresetsSoon();
			});
		connect(deleteBtn, &QPushButton::clicked, this,
			[this, selectedRow, askConfirm, reloadPresetsSoon]() {
				const int i = selectedRow();
				if (!client_ || i < 0)
					return;
				if (!askConfirm(ztr("ZCameraPlugin.Framing.PresetDeleteConfirm")
						    .arg(presetNames_.value(i))))
					return;
				client_->framingPresetDelete(i);
				reloadPresetsSoon();
			});
		connect(presetAddBtn_, &QPushButton::clicked, this,
			[this, reloadPresetsSoon]() {
				if (!client_)
					return;
				client_->framingPresetAdd();
				reloadPresetsSoon();
			});
	}
	layout->addWidget(presetBox);

	connect(restartNowBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		client_->setFramingRestartNow();
		QTimer::singleShot(kFramingReloadMs, this, [this]() { refresh(); });
	});

	connect(applyBtn, &QPushButton::clicked, this, &ZcFramingPanel::apply);
	connect(refreshBtn, &QPushButton::clicked, this, &ZcFramingPanel::refresh);

	/* Remember the construction defaults (restart 30 s etc.) so a camera
	   switch can restore them; see spinDefaults_. */
	for (QSpinBox *spin : findChildren<QSpinBox *>())
		spinDefaults_.insert(spin, spin->value());

	layout->addStretch(1);
}

/* Write every control in one go: the camera stores each one as its own
   /ctrl/framing/<sub> call, exactly like the web UI changing a control. */
void ZcFramingPanel::apply()
{
	if (!client_)
		return;
	client_->setFramingEnabled(detectChk_->isChecked());
	client_->setFramingTrace(traceChk_->isChecked());
	client_->setFramingRestart(restartSpin_->value());
	client_->setFramingAutoTarget(autoTargetChk_->isChecked());
	client_->setFramingZoomLock(zoomLockChk_->isChecked());
	client_->setFramingTiltLock(tiltLockChk_->isChecked());
	client_->setFramingSensitivity(sensitivitySpin_->value());
	client_->setFramingAcceleration(accelerationSpin_->value());
	client_->setFramingScene(sceneCombo_->currentIndex());
	client_->setFramingStartPos(startPosCombo_->currentData().toInt());
	client_->setFramingTargetNum(targetNumCombo_->currentData().toInt());
	client_->setFramingLostAction(lostActionCombo_->currentIndex());
	client_->setFramingLostTimeout(lostTimeoutSpin_->value());
	client_->setFramingWaitTime(waitTimeSpin_->value());
	client_->setFramingMaskEnable(maskChk_->isChecked());
	client_->setFramingTrackActiveFace(faceTrackChk_->isChecked());
	client_->setFramingAutoStart(autoStartChk_->isChecked());
	client_->setFramingTally(tallyChk_->isChecked());
	client_->setFramingTrackingRecord(trackingRecordChk_->isChecked());
	client_->setFramingWaving(wavingChk_->isChecked());
	client_->setFramingGestureFov(gestureFovChk_->isChecked());
	/* The camera may take a moment; re-read to confirm what it stored. */
	QTimer::singleShot(kFramingReloadMs, this, [this]() { refresh(); });
}

void ZcFramingPanel::setClient(ZcCameraClient *client)
{
	client_ = client;
	/* A different camera: reset to the defaults and let the first refresh paint
	   what this camera actually has. */
	if (!client_) {
		for (QCheckBox *chk : findChildren<QCheckBox *>()) {
			QSignalBlocker block(chk);
			chk->setChecked(false);
		}
		for (QSpinBox *spin : findChildren<QSpinBox *>()) {
			QSignalBlocker block(spin);
			spin->setValue(spinDefaults_.value(spin, spin->minimum()));
		}
		for (QComboBox *combo : findChildren<QComboBox *>()) {
			QSignalBlocker block(combo);
			combo->setCurrentIndex(0);
		}
		presetList_->clear();
		presetNames_.clear();
		presetCount_ = 0;
		presetHint_->clear();
		presetAddBtn_->setEnabled(false);
		statusLabel_->setText(ztr("ZCameraPlugin.Framing.Reading"));
		return;
	}
	refresh();
}

void ZcFramingPanel::refresh()
{
	if (!client_ || !client_->isConnected())
		return;
	statusLabel_->setText(ztr("ZCameraPlugin.Framing.Reading"));
	/* One snapshot read carries every framing field. */
	client_->framingQuery(
	    [this](const QJsonObject &doc) { applyStatus(doc); });
	refreshPresets();
}

void ZcFramingPanel::refreshPresets()
{
	if (!client_ || !client_->isConnected())
		return;
	client_->framingPresetQuery(
	    [this](const QJsonObject &doc) { rebuildPresets(doc); });
}

void ZcFramingPanel::rebuildPresets(const QJsonObject &doc)
{
	const QJsonArray presets = doc.value("presets").toArray();
	presetMaxCount_ = doc.value("max_count").toInt();
	presetMaxNameBytes_ = doc.value("max_name_len").toInt();

	const int selected = presetList_->currentRow();
	presetNames_.clear();
	presetList_->clear();
	for (const QJsonValue &v : presets) {
		const QString name = v.toObject().value("name").toString();
		presetNames_ << name;
		presetList_->addItem(name);
	}
	presetCount_ = presetNames_.size();
	if (selected >= 0 && selected < presetCount_)
		presetList_->setCurrentRow(selected);

	/* The camera refuses an add once the table is full. */
	presetAddBtn_->setEnabled(presetMaxCount_ <= 0 ||
				  presetCount_ < presetMaxCount_);
	presetHint_->setText(ztr("ZCameraPlugin.Framing.PresetHint")
				 .arg(presetCount_)
				 .arg(presetMaxCount_));
}

/* Paint the panel from a framing snapshot (/ctrl/framing/all_status) or from a
   live FramingStatusChanged payload. Only the fields actually present are
   touched, so the same method serves both: the live payload carries the two
   switches but not the stored settings. A flag can arrive as a bool or as 1/0
   depending on the endpoint, so both are accepted. Never stored — the panel
   only ever shows what the camera has. */
void ZcFramingPanel::applyStatus(const QJsonObject &doc)
{
	const auto asBool = [&doc](const char *key, bool fallback) {
		const QJsonValue v = doc.value(QLatin1String(key));
		if (v.isBool())
			return v.toBool();
		if (v.isDouble())
			return v.toInt() != 0;
		return fallback;
	};
	const auto asInt = [&doc](const char *key, int fallback) {
		const QJsonValue v = doc.value(QLatin1String(key));
		if (v.isDouble())
			return v.toInt();
		if (v.isBool())
			return v.toBool() ? 1 : 0;
		return fallback;
	};
	const auto bindBool = [&doc, &asBool](const char *key, QCheckBox *chk) {
		if (chk && doc.contains(QLatin1String(key))) {
			QSignalBlocker block(chk);
			chk->setChecked(asBool(key, false));
		}
	};
	const auto bindInt = [&doc, &asInt](const char *key, QSpinBox *spin,
					    int min, int max) {
		if (spin && doc.contains(QLatin1String(key))) {
			QSignalBlocker block(spin);
			spin->setValue(qBound(min, asInt(key, min), max));
		}
	};

	bindBool("detect", detectChk_);
	bindBool("trace", traceChk_);
	bindBool("auto_target", autoTargetChk_);
	bindBool("zoom_lock", zoomLockChk_);
	bindBool("tilt_lock", tiltLockChk_);
	bindBool("track_active_face", faceTrackChk_);
	bindBool("auto_start", autoStartChk_);
	bindBool("tally", tallyChk_);
	bindBool("tracking_record", trackingRecordChk_);
	bindBool("maskarea", maskChk_);
	bindBool("waving", wavingChk_);
	if (doc.contains("fullbody") || doc.contains("waist") ||
	    doc.contains("closeup")) {
		/* The web UI shows the three FOV switches as one segment, so the
		   checkbox is on when any of them is. */
		QSignalBlocker block(gestureFovChk_);
		gestureFovChk_->setChecked(asBool("fullbody", false) ||
					   asBool("waist", false) ||
					   asBool("closeup", false));
	}

	bindInt("detect_restart", restartSpin_, 0, 600);
	bindInt("lost_timeout", lostTimeoutSpin_, 0, 60);
	bindInt("wait_time", waitTimeSpin_, 0, 60);
	bindInt("sensitivity", sensitivitySpin_, kFramingSpeedMin,
		kFramingSpeedMax);
	bindInt("acceleration", accelerationSpin_, kFramingSpeedMin,
		kFramingSpeedMax);

	if (doc.contains("scene")) {
		QSignalBlocker block(sceneCombo_);
		sceneCombo_->setCurrentIndex(
		    qBound(0, asInt("scene", 0),
			   int(sizeof(kFramingScenes) / sizeof(kFramingScenes[0])) - 1));
	}
	if (doc.contains("lost_action")) {
		QSignalBlocker block(lostActionCombo_);
		lostActionCombo_->setCurrentIndex(
		    qBound(0, asInt("lost_action", 0),
			   int(sizeof(kFramingLostActions) /
			       sizeof(kFramingLostActions[0])) -
			       1));
	}
	if (doc.contains("target_num")) {
		QSignalBlocker block(targetNumCombo_);
		const int idx =
		    targetNumCombo_->findData(asInt("target_num", kFramingTargetNumMin));
		if (idx >= 0)
			targetNumCombo_->setCurrentIndex(idx);
	}
	if (doc.contains("start_pos") || doc.contains("start_pos_opts")) {
		/* The camera names the slots itself (start_pos_opts) and pads the list
		   with empty strings, so fall back to the built-in labels. */
		const QJsonArray opts = doc.value("start_pos_opts").toArray();
		const int fallbackCount =
		    int(sizeof(kFramingStartPosFallback) /
			sizeof(kFramingStartPosFallback[0]));
		const int count = opts.isEmpty() ? fallbackCount : int(opts.size());
		QSignalBlocker block(startPosCombo_);
		startPosCombo_->clear();
		for (int i = 0; i < count; i++) {
			QString label =
			    i < opts.size() ? opts.at(i).toString().trimmed() : QString();
			if (label.isEmpty() && i < fallbackCount)
				label = QString::fromUtf8(kFramingStartPosFallback[i]);
			if (label.isEmpty())
				label = QString::number(i);
			startPosCombo_->addItem(label, i);
		}
		const int idx = startPosCombo_->findData(asInt("start_pos", 0));
		if (idx >= 0)
			startPosCombo_->setCurrentIndex(idx);
	}

	updateStatusLabel();
}

void ZcFramingPanel::updateStatusLabel()
{
	const auto onOff = [this](bool on) {
		return on ? ztr("ZCameraPlugin.Framing.On")
			  : ztr("ZCameraPlugin.Framing.Off");
	};
	statusLabel_->setText(ztr("ZCameraPlugin.Framing.Status")
				      .arg(onOff(detectChk_->isChecked()))
				      .arg(onOff(traceChk_->isChecked()))
				      .arg(restartSpin_->value()));
}

/* ---------------------------------------------------------------- */
/* ZcPresetPanel                                                     */
/* ---------------------------------------------------------------- */

namespace {

/* Slot layout, matching the camera's own web UI preset page. */
constexpr int kPresetPerPage = 10;
constexpr int kPresetPages = 10;
/* The camera rejects names shorter than 1 or longer than 18 characters. */
constexpr int kPresetNameMax = 18;
/* Thumbnails are written asynchronously; re-read the slot a little later. */
constexpr int kPresetReloadMs = 400;

/* A preset slot's clickable frame. A QFrame subclass (not a plain widget with
   an event filter) so the click is caught even though the thumbnail and name
   child widgets cover the whole surface. */
class ZcPresetSlotFrame : public QFrame {
public:
	std::function<void()> onClicked;

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && onClicked)
			onClicked();
		event->accept();
	}
};

const QString kPresetPanelStyle =
    "QWidget#PresetPanel { background:#1f2927; }"
    "QLabel { color:#8a9bb2; font-size:11px; letter-spacing:1px; }"
    "QPushButton { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:4px 8px; font-size:11px; }"
    "QPushButton:hover { background:#354845; }"
    "QPushButton:pressed { background:#d8b878; color:#1f2927; }"
    "QPushButton:disabled { color:#6b7a90; background:#232d2b; "
    "border-color:#2f3d3b; }"
    "QPushButton:checked { background:#d8b878; color:#1f2927; }"
    "QComboBox { color:#eef2ed; background:#2b3a38; border:1px solid "
    "#3c514e; border-radius:4px; padding:2px 6px; font-size:11px; }"
    "QComboBox:disabled { color:#6b7a90; }";

} // namespace

ZcPresetPanel::ZcPresetPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("PresetPanel");
	setStyleSheet(kPresetPanelStyle);
	buildUi();
	/* The camera's trace transport advances on its own (a playback ends without
	   the operator clicking Stop). Poll its state while it is busy so the buttons
	   follow the camera (bug 24). Started/stopped in onTraceState(). */
	tracePollTimer_ = new QTimer(this);
	tracePollTimer_->setInterval(800);
	connect(tracePollTimer_, &QTimer::timeout, this,
		&ZcPresetPanel::refreshTraceState);
}

void ZcPresetPanel::buildUi()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(6);

	/* Family selector: the camera keeps presets and traces (trajectories) in
	   two separate slot families, both addressed with the same 0..99 index
	   (spec §3). */
	auto *modeRow = new QHBoxLayout;
	modeRow->setSpacing(3);
	presetModeBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Preset"), this);
	traceModeBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Trace"), this);
	for (QPushButton *btn : {presetModeBtn_, traceModeBtn_}) {
		btn->setCheckable(true);
		btn->setFixedWidth(64);
		modeRow->addWidget(btn);
	}
	presetModeBtn_->setChecked(true);
	modeRow->addStretch(1);
	modeLabel_ = new QLabel(ztr("ZCameraPlugin.Dock.Presets"), this);
	modeRow->addWidget(modeLabel_);
	layout->addLayout(modeRow);

	connect(presetModeBtn_, &QPushButton::clicked, this,
		[this]() { setTraceMode(false); });
	connect(traceModeBtn_, &QPushButton::clicked, this,
		[this]() { setTraceMode(true); });

	/* Page selector: one button per page of 10 slots. */
	auto *pageRow = new QHBoxLayout;
	pageRow->setSpacing(3);
	for (int p = 0; p < kPresetPages; ++p) {
		auto *btn = new QPushButton(QString::number(p + 1), this);
		btn->setCheckable(true);
		btn->setFixedWidth(28);
		connect(btn, &QPushButton::clicked, this,
			[this, p]() { showPage(p); });
		pageRow->addWidget(btn);
		pageButtons_.append(btn);
	}
	pageRow->addStretch(1);
	layout->addLayout(pageRow);

	/* Slot grid, 5 columns x 2 rows. */
	auto *grid = new QGridLayout;
	grid->setSpacing(6);
	for (int s = 0; s < kPresetPerPage; ++s) {
		auto *frame = new ZcPresetSlotFrame;
		frame->setObjectName("PresetSlot");
		frame->setFixedSize(116, 88);
		frame->setCursor(Qt::PointingHandCursor);
		frame->onClicked = [this, s]() {
			onSlotClicked(page_ * kPresetPerPage + s);
		};

		auto *fl = new QVBoxLayout(frame);
		fl->setContentsMargins(4, 4, 4, 4);
		fl->setSpacing(2);

		auto *thumb = new QLabel(frame);
		thumb->setFixedSize(106, 56);
		thumb->setAlignment(Qt::AlignCenter);
		thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		thumb->setStyleSheet("color:#6b7a90;font-size:10px;");

		auto *name = new QLineEdit(frame);
		name->setReadOnly(true);
		name->setMaxLength(kPresetNameMax);
		name->setAlignment(Qt::AlignCenter);
		name->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		name->setStyleSheet(
		    "QLineEdit { color:#eef2ed; background:transparent; border:none;"
		    " font-size:10px; }"
		    "QLineEdit:!read-only { background:#2b3a38; border:1px solid "
		    "#d8b878; }");
		/* `&` and `=` would break the query string of set_name. */
		connect(name, &QLineEdit::textChanged, this,
			[name](const QString &text) {
				if (!text.contains('&') && !text.contains('='))
					return;
				QString cleaned = text;
				cleaned.remove('&');
				cleaned.remove('=');
				const int pos = name->cursorPosition();
				QSignalBlocker block(name);
				name->setText(cleaned);
				name->setCursorPosition(qMin(pos, cleaned.size()));
			});
		connect(name, &QLineEdit::returnPressed, this, [this, s]() {
			commitRename(page_ * kPresetPerPage + s,
				     slots_[s].name->text());
		});
		connect(name, &QLineEdit::editingFinished, this, [this, s]() {
			if (!slots_[s].name->isReadOnly())
				commitRename(page_ * kPresetPerPage + s,
					     slots_[s].name->text());
		});

		fl->addWidget(thumb);
		fl->addWidget(name);
		slots_.append(Slot{frame, thumb, name});
		grid->addWidget(frame, s / 5, s % 5);
	}
	layout->addLayout(grid);

	hintLabel_ = new QLabel("", this);
	hintLabel_->setStyleSheet("color:#d8b878;font-size:10px;");
	layout->addWidget(hintLabel_);

	/* Per-preset recall speed. Traces have no speed/time of their own, so the
	   whole row is hidden in trace mode. */
	speedRow_ = new QWidget(this);
	auto *speedRow = new QHBoxLayout(speedRow_);
	speedRow->setContentsMargins(0, 0, 0, 0);
	speedRow->setSpacing(6);
	speedRow->addWidget(new QLabel(ztr("ZCameraPlugin.Dock.RecallSpeed"), speedRow_));
	unitCombo_ = new QComboBox(speedRow_);
	unitCombo_->addItem(ztr("ZCameraPlugin.Dock.Speed"));
	unitCombo_->addItem(ztr("ZCameraPlugin.Dock.Time"));
	speedRow->addWidget(unitCombo_);
	speedCombo_ = new QComboBox(speedRow_);
	speedCombo_->setMinimumWidth(70);
	speedRow->addWidget(speedCombo_);
	speedRow->addStretch(1);
	layout->addWidget(speedRow_);

	/* Actions on the selected slot. The first two buttons carry a different
	   meaning per family: Recall / Add-Replace for presets, Record /
	   Prepare-Play-Stop for traces. */
	auto *actionRow = new QHBoxLayout;
	actionRow->setSpacing(6);
	primaryBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Recall"), this);
	secondaryBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Add"), this);
	deleteBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Delete"), this);
	renameBtn_ = new QPushButton(ztr("ZCameraPlugin.Dock.Rename"), this);
	actionRow->addWidget(primaryBtn_);
	actionRow->addWidget(secondaryBtn_);
	actionRow->addWidget(deleteBtn_);
	actionRow->addWidget(renameBtn_);
	actionRow->addStretch(1);
	layout->addLayout(actionRow);
	layout->addStretch(1);

	connect(primaryBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		if (!traceMode_) {
			if (selected_ >= 0)
				client_->ptzPresetRecall(selected_);
			return;
		}
		/* Trace mode: the button starts a recording, or stops the one that
		   is running. */
		if (traceStateFromInt(traceState_) == TraceState::Recording) {
			client_->ptraceRecordStop();
			QTimer::singleShot(kPresetReloadMs, this, [this]() {
				refreshTraceState();
				loadSlot(selected_);
			});
			return;
		}
		if (selected_ < 0)
			return;
		const int index = selected_;
		/* The camera overwrites the slot, so its cached name and thumbnail
		   are stale the moment the recording starts. */
		thumbs_.remove(index);
		info_[index].insert("name", QString());
		slots_[index % kPresetPerPage].name->setText(defaultName(index));
		client_->ptraceRecordStart(index);
		QTimer::singleShot(kPresetReloadMs, this, [this, index]() {
			refreshTraceState();
			loadSlot(index);
		});
	});
	connect(secondaryBtn_, &QPushButton::clicked, this, [this]() {
		if (!client_)
			return;
		if (!traceMode_) {
			if (selected_ < 0)
				return;
			const int index = selected_;
			/* The stored thumbnail is now stale; re-read once the camera
			   has written the new one. */
			thumbs_.remove(index);
			client_->ptzPresetSet(index);
			QTimer::singleShot(kPresetReloadMs, this,
					   [this, index]() { loadSlot(index); });
			return;
		}
		switch (traceStateFromInt(traceState_)) {
		case TraceState::Playing:
			client_->ptracePlayStop();
			break;
		case TraceState::ReadyForPlay:
			client_->ptracePlayStart();
			break;
		default:
			if (selected_ < 0)
				return;
			client_->ptracePlayPrepare(selected_);
			break;
		}
		QTimer::singleShot(kPresetReloadMs, this,
				   [this]() { refreshTraceState(); });
	});
	connect(deleteBtn_, &QPushButton::clicked, this, [this]() {
		if (selected_ < 0 || !client_)
			return;
		const int index = selected_;
		thumbs_.remove(index);
		info_[index].insert("name", QString());
		if (traceMode_)
			client_->ptraceDelete(index);
		else
			client_->ptzPresetDelete(index);
		QTimer::singleShot(kPresetReloadMs, this,
				   [this, index]() { loadSlot(index); });
	});
	connect(renameBtn_, &QPushButton::clicked, this,
		&ZcPresetPanel::beginRename);

	connect(unitCombo_, &QComboBox::currentIndexChanged, this,
		[this](int unit) {
			if (selected_ < 0 || !client_)
				return;
			client_->ptzPresetSetSpeedUnit(selected_, unit);
			info_[selected_].insert("unit", unit);
			updateSpeedControls();
		});
	connect(speedCombo_, &QComboBox::currentIndexChanged, this,
		[this](int row) {
			if (selected_ < 0 || !client_ || row < 0)
				return;
			const int value = speedCombo_->itemData(row).toInt();
			if (unitCombo_->currentIndex() == 1) {
				client_->ptzPresetSetSpeedByDuration(selected_,
								     value);
				info_[selected_].insert("time", value * 1000);
			} else {
				client_->ptzPresetSetSpeedByIndex(selected_, value);
				info_[selected_].insert("speed", value);
			}
		});

	showPage(0);
}

void ZcPresetPanel::setClient(ZcCameraClient *client)
{
	client_ = client;
	if (client_)
		/* The per-preset speed unit / speed follow `ptz_speed_mode`: with the
		   "Common" preset speed mode the camera applies one shared speed to
		   every preset, so the per-preset controls are refused. The PTZ catalog
		   carries that key, so repaint the row's enablement whenever it is
		   refreshed (bug list: the preset speed unit must follow the PTZ preset
		   speed mode). */
		connect(client_, &ZcCameraClient::groupRefreshed, this,
			[this](const QString &catalog) {
				if (catalog == QLatin1String("ptz"))
					updateActions();
			});
	refresh();
}

void ZcPresetPanel::refresh()
{
	/* A different camera: drop everything cached and start over. */
	info_.clear();
	thumbs_.clear();
	maxSpeed_ = 0;
	maxTime_ = 0;
	traceState_ = 0;
	if (tracePollTimer_)
		tracePollTimer_->stop();
	showPage(page_);

	if (!client_)
		return;
	if (traceMode_)
		refreshTraceState();
	/* The speed/time menus are bounded by the camera's own maxima. */
	client_->readSetting("ptz_common_speed", [this](const ZcSettingValue &v) {
		maxSpeed_ = v.max > 0 ? static_cast<int>(v.max) : 0;
		updateSpeedControls();
	});
	client_->readSetting("ptz_common_time", [this](const ZcSettingValue &v) {
		maxTime_ = v.max > 0 ? static_cast<int>(v.max) : 0;
		updateSpeedControls();
	});
}

/* Spec §3: presets and traces are two independent slot families. Switching
   family is a full reload — they share the 0..99 index space but no entry. */
void ZcPresetPanel::setTraceMode(bool trace)
{
	if (traceMode_ == trace)
		return;
	traceMode_ = trace;
	info_.clear();
	thumbs_.clear();
	traceState_ = 0;
	if (tracePollTimer_)
		tracePollTimer_->stop();
	presetModeBtn_->setChecked(!trace);
	traceModeBtn_->setChecked(trace);
	modeLabel_->setText(trace ? ztr("ZCameraPlugin.Dock.Traces") : ztr("ZCameraPlugin.Dock.Presets"));
	speedRow_->setVisible(!trace);
	showPage(0);
	if (trace)
		refreshTraceState();
}

void ZcPresetPanel::refreshTraceState()
{
	if (!traceMode_ || !client_ || !client_->isConnected())
		return;
	client_->ptraceQuery([this](int stCode) {
		if (traceMode_)
			onTraceState(stCode);
	});
}

void ZcPresetPanel::onTraceState(int stCode)
{
	/* -1 means the camera did not answer; keep the last known state rather
	   than re-enabling buttons that may not be actionable. */
	if (stCode < 0)
		return;
	traceState_ = stCode;
	updateActions();

	/* The trace transport runs on the camera and advances on its own: while it
	   is busy (recording / preparing / playing / deleting) keep polling so the
	   buttons follow it — e.g. a playback that ends returns to "Prepare" without
	   the operator clicking Stop (bug 24). Once it is idle the poll is dropped. */
	const TraceState state = traceStateFromInt(stCode);
	const bool busy = (state == TraceState::Recording ||
			   state == TraceState::Preparing ||
			   state == TraceState::Playing ||
			   state == TraceState::Deleting);
	if (tracePollTimer_ && busy && !tracePollTimer_->isActive())
		tracePollTimer_->start();
	else if (tracePollTimer_ && !busy)
		tracePollTimer_->stop();
}

QString ZcPresetPanel::defaultName(int index) const
{
	/* The wire index is 0-based, the label 1-based (spec §3). */
	return (traceMode_ ? ztr("ZCameraPlugin.Dock.TraceN") : ztr("ZCameraPlugin.Dock.PresetN"))
	    .arg(index + 1, 3, 10, QChar('0'));
}

bool ZcPresetPanel::selectedIsNamed() const
{
	if (selected_ < 0)
		return false;
	return !info_.value(selected_).value("name").toString().isEmpty();
}

void ZcPresetPanel::showPage(int page)
{
	if (page < 0 || page >= kPresetPages)
		return;
	page_ = page;
	selected_ = -1;
	for (int p = 0; p < pageButtons_.size(); ++p)
		pageButtons_[p]->setChecked(p == page);

	/* Reset the visible slots before the replies land, so a page switch never
	   shows the previous page's names or thumbnails. */
	for (int s = 0; s < kPresetPerPage; ++s) {
		slots_[s].thumb->setPixmap(QPixmap());
		slots_[s].thumb->setText("");
		slots_[s].name->setText(defaultName(page * kPresetPerPage + s));
	}
	hintLabel_->clear();
	updateSelection();
	updateActions();
	updateSpeedControls();

	for (int s = 0; s < kPresetPerPage; ++s)
		loadSlot(page * kPresetPerPage + s);
}

void ZcPresetPanel::loadSlot(int index)
{
	if (!client_ || !client_->isConnected())
		return;
	const bool trace = traceMode_;
	/* A reply that arrives after the user switched family must not overwrite
	   the other family's slot. */
	auto apply = [this, index, trace](const QJsonObject &info) {
		if (trace != traceMode_)
			return;
		applySlotInfo(index, info);
	};
	if (trace)
		client_->ptraceInfo(index, apply);
	else
		client_->ptzPresetInfo(index, apply);
}

void ZcPresetPanel::applySlotInfo(int index, const QJsonObject &info)
{
	/* A late reply for a page the user already left must not repaint it. */
	if (index / kPresetPerPage != page_)
		return;
	const bool trace = traceMode_;
	info_[index] = info;

	Slot &slot = slots_[index % kPresetPerPage];
	const QString name = info.value("name").toString();
	slot.name->setText(name.isEmpty() ? defaultName(index) : name);

	if (index == selected_) {
		updateActions();
		updateSpeedControls();
	}
	/* A non-zero status means the camera is mid-move; warn rather than
	   letting the operator recall into a moving head. Presets only — a trace
	   reply carries no status. The warning must also CLEAR: the next reply
	   for the selected slot with status back to 0 means the head settled,
	   and a stuck notice would keep warning about a move that ended. */
	if (!trace) {
		if (info.value("status").toInt(0) != 0) {
			hintLabel_->setText(ztr("ZCameraPlugin.Dock.PresetBusy"));
		} else if (index == selected_ &&
			   hintLabel_->text() ==
				   ztr("ZCameraPlugin.Dock.PresetBusy")) {
			hintLabel_->clear();
		}
	}

	if (name.isEmpty()) {
		slot.thumb->setPixmap(QPixmap());
		slot.thumb->setText("");
		return;
	}
	if (thumbs_.contains(index)) {
		slot.thumb->setPixmap(thumbs_.value(index));
		return;
	}
	slot.thumb->setText(ztr("ZCameraPlugin.Dock.Ellipsis"));
	auto got = [this, index, trace](const QByteArray &bytes) {
		if (trace != traceMode_)
			return;
		QPixmap pm;
		if (!bytes.isEmpty())
			pm.loadFromData(bytes);
		if (!pm.isNull())
			thumbs_.insert(index, pm);
		if (index / kPresetPerPage != page_)
			return;
		Slot &s = slots_[index % kPresetPerPage];
		if (pm.isNull()) {
			/* No thumbnail for this slot: the name alone identifies it. */
			s.thumb->setText("");
			return;
		}
		s.thumb->setText("");
		s.thumb->setPixmap(pm.scaled(s.thumb->size(),
					     Qt::KeepAspectRatio,
					     Qt::SmoothTransformation));
	};
	if (trace)
		client_->ptraceThumbnail(index, got);
	else
		client_->ptzPresetThumbnail(index, got);
}

void ZcPresetPanel::onSlotClicked(int index)
{
	/* While a trace is recording or playing the camera owns the transport;
	   moving the selection to another slot would target a different take. */
	if (traceMode_ && !traceActions(traceStateFromInt(traceState_)).navigate)
		return;
	/* Click toggles: clicking the selected slot again clears the selection,
	   and no action is available while nothing is selected (spec §3). */
	selected_ = (selected_ == index) ? -1 : index;
	updateSelection();
	updateActions();
	updateSpeedControls();
}

void ZcPresetPanel::updateSelection()
{
	for (int s = 0; s < slots_.size(); ++s) {
		const bool selected =
		    (page_ * kPresetPerPage + s) == selected_;
		slots_[s].frame->setStyleSheet(
		    selected
			? "QFrame#PresetSlot { border:2px solid #d8b878; "
			  "border-radius:4px; background:#22302e; }"
			: "QFrame#PresetSlot { border:1px solid #3c514e; "
			  "border-radius:4px; background:#1a2321; }");
		/* Leaving a slot always ends an in-progress rename. */
		slots_[s].name->setReadOnly(true);
		slots_[s].name->setAttribute(Qt::WA_TransparentForMouseEvents,
					     true);
	}
}

void ZcPresetPanel::updateActions()
{
	const bool has = selected_ >= 0;
	const bool named = selectedIsNamed();

	if (!traceMode_) {
		primaryBtn_->setText(ztr("ZCameraPlugin.Dock.Recall"));
		/* Recall / Delete / Rename act on a preset that already exists, so a
		   selected-but-empty slot must grey them; "Add" writes a new preset and
		   stays available (the camera's web UI toggles Add/Replace similarly). */
		primaryBtn_->setEnabled(named);
		/* "Add" writes a new preset, "Replace" overwrites a named one — the
		   same label toggle the camera's web UI uses. */
		secondaryBtn_->setText(named ? ztr("ZCameraPlugin.Dock.Replace") : ztr("ZCameraPlugin.Dock.Add"));
		secondaryBtn_->setEnabled(has);
		deleteBtn_->setEnabled(named);
		renameBtn_->setEnabled(named);
		/* With the PTZ preset speed mode "Common" the camera recalls every
		   preset at one shared speed, so a per-preset unit/speed is refused; the
		   camera's web UI greys its "Unit"/"Speed" entries in exactly this case.
		   An unread value (empty) leaves them usable rather than over-disabling
		   the row. */
		const bool perPresetSpeed =
		    !client_ ||
		    client_->settingValue(QLatin1String("ptz_speed_mode")) !=
			QLatin1String("Common");
		unitCombo_->setEnabled(named && perPresetSpeed);
		speedCombo_->setEnabled(named && perPresetSpeed &&
					speedCombo_->count() > 0);
		for (QPushButton *btn : pageButtons_)
			btn->setEnabled(true);
		return;
	}

	/* Trace mode. Which control is actionable depends on where the camera's
	   trace transport currently is (spec §3). */
	const TraceState state = traceStateFromInt(traceState_);
	const TraceActions act = traceActions(state);

	primaryBtn_->setText(state == TraceState::Recording ? ztr("ZCameraPlugin.Dock.StopRecord")
							   : ztr("ZCameraPlugin.Dock.StartRecord"));
	/* While recording the button is the stop control, so it needs no slot
	   selection; otherwise a slot must be selected to record into. */
	primaryBtn_->setEnabled(act.record &&
				(has || state == TraceState::Recording));

	secondaryBtn_->setText(state == TraceState::Playing
				   ? ztr("ZCameraPlugin.Dock.StopPlayback")
				   : state == TraceState::ReadyForPlay ? ztr("ZCameraPlugin.Dock.Play")
								      : ztr("ZCameraPlugin.Dock.Prepare"));
	/* Playing and ready-to-play act on the trace the camera already holds,
	   not on the selection. */
	secondaryBtn_->setEnabled(act.prepare &&
				  (named || state == TraceState::Playing ||
				   state == TraceState::ReadyForPlay));

	deleteBtn_->setEnabled(act.remove && named);
	/* Renaming writes into a slot, so it needs a selection and an idle
	   transport. */
	renameBtn_->setEnabled(has && act.navigate);

	unitCombo_->setEnabled(false);
	speedCombo_->setEnabled(false);
	for (QPushButton *btn : pageButtons_)
		btn->setEnabled(act.navigate);

	switch (state) {
	case TraceState::Recording:
		hintLabel_->setText(
		    ztr("ZCameraPlugin.Dock.TraceRecording"));
		break;
	case TraceState::ReadyForPlay:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceReady"));
		break;
	case TraceState::Playing:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TracePlaying"));
		break;
	case TraceState::Preparing:
	case TraceState::Deleting:
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.TraceBusy"));
		break;
	default:
		hintLabel_->clear();
		break;
	}
}

void ZcPresetPanel::updateSpeedControls()
{
	/* Trace slots carry no unit/speed fields; the combos are disabled in
	   trace mode anyway (updateActions), and repainting them from a trace
	   reply would blank the preset family's selections while the user is
	   only browsing traces. */
	if (traceMode_)
		return;
	if (selected_ < 0) {
		QSignalBlocker block(speedCombo_);
		speedCombo_->clear();
		return;
	}
	const QJsonObject info = info_.value(selected_);
	const int unit = info.value("unit").toInt(0);
	{
		QSignalBlocker block(unitCombo_);
		unitCombo_->setCurrentIndex(unit == 1 ? 1 : 0);
	}

	QSignalBlocker block(speedCombo_);
	speedCombo_->clear();
	if (unit == 1) {
		/* Time: the camera stores milliseconds; offer whole seconds. */
		const int maxSeconds = maxTime_ / 1000;
		for (int s = 1; s <= maxSeconds; ++s)
			speedCombo_->addItem(ztr("ZCameraPlugin.Dock.Seconds").arg(s), s);
		const int current = info.value("time").toInt() / 1000;
		if (current > 0 && speedCombo_->count() > 0)
			speedCombo_->setCurrentIndex(
			    qBound(0, current - 1, speedCombo_->count() - 1));
	} else {
		for (int s = 1; s <= maxSpeed_; ++s)
			speedCombo_->addItem(QString::number(s), s);
		const int current = info.value("speed").toInt();
		if (current > 0 && speedCombo_->count() > 0)
			speedCombo_->setCurrentIndex(
			    qBound(0, current - 1, speedCombo_->count() - 1));
	}
	speedCombo_->setEnabled(speedCombo_->count() > 0);
}

void ZcPresetPanel::beginRename()
{
	if (selected_ < 0)
		return;
	Slot &slot = slots_[selected_ % kPresetPerPage];
	slot.name->setAttribute(Qt::WA_TransparentForMouseEvents, false);
	slot.name->setReadOnly(false);
	slot.name->setFocus();
	slot.name->selectAll();
}

void ZcPresetPanel::commitRename(int index, const QString &text)
{
	Slot &slot = slots_[index % kPresetPerPage];
	slot.name->setReadOnly(true);
	slot.name->setAttribute(Qt::WA_TransparentForMouseEvents, true);

	QString name = text;
	name.remove('&');
	name.remove('=');
	const QString stored = info_.value(index).value("name").toString();

	/* 1..18 bytes; anything else reverts to the stored name. The camera
	   counts UTF-8 BYTES (same limit the framing name clamp uses), so a
	   Chinese name of 18 characters is over the limit and must be rejected
	   here rather than silently truncated by the camera. */
	if (name.isEmpty() || name.toUtf8().size() > kPresetNameMax) {
		slot.name->setText(stored.isEmpty() ? defaultName(index) : stored);
		return;
	}
	if (name == stored)
		return;

	if (!client_)
		return;
	if (traceMode_)
		client_->ptraceSetName(index, name);
	else
		client_->ptzPresetSetName(index, name);
	/* The camera does not echo the name back, so show it now and re-read to
	   confirm it stuck. */
	info_[index].insert("name", name);
	slot.name->setText(name);
	if (index == selected_)
		updateActions();
	loadSlot(index);
}

/* ---------------------------------------------------------------- */
/* ZcControlDock                                                     */
/* ---------------------------------------------------------------- */

/* Whether an auto-framing status payload means framing is driving the head. The
   camera reports it as `value` (overall) and/or `detect` (the detection switch);
   either being on means presets/traces cannot move it (bug 47; the RD comment
   supplied the exact websocket samples). */
static bool framingActive(const QJsonObject &status)
{
	const auto on = [&status](const char *key) {
		const QJsonValue v = status.value(QLatin1String(key));
		if (v.isBool())
			return v.toBool();
		if (v.isDouble())
			return v.toInt() != 0;
		return false;
	};
	return on("value") || on("detect");
}

/* Set by ~ZcControlDock. A connect probe runs a nested event loop inside
   client_->connect(); OBS can tear the dock down from an event delivered in that
   loop, leaving the probe's frames on the stack. When the loop unwinds they
   resume (negotiateOrigin -> openCameraNow -> openCamera -> the click slot)
   against the already-freed dock, so each resume point checks this and bails out
   before dereferencing a freed member. */
static bool g_controlDockClosing = false;

ZcControlDock::ZcControlDock(QWidget *parent) : QDockWidget(parent)
{
	/* A fresh dock: clear any flag left by a previous instance's teardown. */
	g_controlDockClosing = false;
	setWindowTitle(QString::fromUtf8(
	    obs_module_text("ZCameraPlugin.Dock.Title")));
	setObjectName("zcamera-control-dock");
	setAllowedAreas(Qt::AllDockWidgetAreas);
	/* The settings pane needs room for a label plus a combo/slider per row;
	   at the default size the controls would be clipped. */
	resize(1040, 640);

	auto *container = new QWidget(this);
	auto *outer = new QVBoxLayout(container);
	outer->setContentsMargins(0, 0, 0, 0);

	splitter_ = new QSplitter(Qt::Horizontal, container);
	rail_ = new ZcCameraRail(splitter_);

	auto *right = new QWidget(splitter_);
	auto *rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);

	/* Right side is a two-tab pane: Settings (existing panel) and PTZ. */
	tabs_ = new QTabWidget(right);
	tabs_->setDocumentMode(true);
	tabs_->setStyleSheet(
	    "QTabWidget::pane { border:none; background:#1f2927; }"
	    "QTabBar::tab { color:#8a9bb2; background:#22302e; padding:7px 16px;"
	    " border:1px solid #3c514e; border-bottom:none; font-size:11px;"
	    " letter-spacing:1px; }"
	    "QTabBar::tab:selected { color:#eef2ed; background:#1f2927;"
	    " border-top:2px solid #d8b878; }"
	    "QTabBar::tab:hover:!selected { color:#eef2ed; }");
	settings_ = new ZcSettingsPanel(tabs_);
	tabs_->addTab(settings_, ztr("ZCameraPlugin.Dock.Settings"));

	/* The PTZ pane (joystick + preset list) is built up front but only
	   inserted into the tab bar once a camera reports PTZ support (spec §3).
	   It must stay hidden until then: a widget parented to the QTabWidget
	   without being a tab is a plain child, so it would be painted on top of
	   the current page (it used to cover Settings until the first camera was
	   selected). addTab() reparents it into the stack, which shows it on
	   selection. */
	ptzTab_ = new QWidget(tabs_);
	ptzTab_->hide();
	auto *ptzLayout = new QHBoxLayout(ptzTab_);
	ptzLayout->setContentsMargins(0, 0, 0, 0);
	ptzLayout->setSpacing(0);
	/* The pad grew tall (joystick + position + the four range-limit rows), so it
	   scrolls rather than squeezing the joystick on a short dock. The scroll area
	   keeps the pad's fixed width, so the presets keep the rest. */
	ptzScroll_ = new QScrollArea(ptzTab_);
	ptzScroll_->setWidgetResizable(true);
	ptzScroll_->setFrameShape(QFrame::NoFrame);
	ptzScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ptzScroll_->setFixedWidth(268);
	ptzScroll_->viewport()->setStyleSheet("background:#1f2927;");
	ptz_ = new ZcPtzPad();
	ptzScroll_->setWidget(ptz_);
	presets_ = new ZcPresetPanel(ptzTab_);
	ptzLayout->addWidget(ptzScroll_, 0);
	ptzLayout->addWidget(presets_, 1);
	rightLayout->addWidget(tabs_, 1);

	/* The auto-framing tab is built up front and hidden like the PTZ tab; it is
	   inserted into the tab bar once a camera reports auto framing (see
	   kFramingPanelVisible in updatePtzTab). Building it here keeps the widget,
	   its client calls and its WebSocket handlers alive. */
	framingTab_ = new QWidget(tabs_);
	framingTab_->hide();
	auto *framingLayout = new QVBoxLayout(framingTab_);
	framingLayout->setContentsMargins(0, 0, 0, 0);
	framing_ = new ZcFramingPanel(framingTab_);
	framingLayout->addWidget(framing_, 1);

	splitter_->addWidget(rail_);
	splitter_->addWidget(right);
	splitter_->setStretchFactor(0, 0);
	splitter_->setStretchFactor(1, 1);
	outer->addWidget(splitter_, 1);

	/* Status bar. */
	auto *statusBar = new QHBoxLayout;
	statusBarLabel_ = new QLabel(ztr("ZCameraPlugin.Dock.StatusDisconnected"), container);
	statusBarLabel_->setStyleSheet("color:#8a9bb2;padding:2px 6px;");
	statusBar->addWidget(statusBarLabel_);
	statusBar->addStretch();
#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Development aid: only compiled in when the build asks for it, so a
	   release build ships no debug affordance (bug list: "Debug button not
	   removed from the release build"). */
	debugButton_ = new QPushButton(ztr("ZCameraPlugin.Dock.Debug"), container);
	debugButton_->setCheckable(true);
	statusBar->addWidget(debugButton_);
#endif
	outer->addLayout(statusBar);

	setWidget(container);

#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Debug panel (dockable/floating, hidden by default). */
	debugPanel_ = new ZcDebugPanel(nullptr);
	debugPanel_->hide();

	connect(debugButton_, &QPushButton::toggled, this, [this](bool on) {
		if (on)
			debugPanel_->show();
		else
			debugPanel_->hide();
	});
#endif

	connect(rail_, &ZcCameraRail::cameraClicked, this,
		&ZcControlDock::onCameraClicked);
	/* A row's own chip: the only path that touches the scene (spec §1,
	   L2/L6). It names its camera, so nothing has to be selected first. */
	connect(rail_, &ZcCameraRail::addRequested, this,
		[this](const QString &host) {
			addCameraToSource(host);
			refreshRailPresence();
		});
	/* A gold check on an already-added row removes the source from OBS. */
	connect(rail_, &ZcCameraRail::removeRequested, this,
		[this](const QString &host) {
			removeCameraFromSource(host);
			refreshRailPresence();
		});
	connect(ptz_, &ZcPtzPad::homeRequested, this,
		[this]() {
			if (client_)
				client_->ptzHome();
		});

	/* Presence is evaluated live, never cached (spec §1, L4): watch libobs's
	   own source create/destroy/rename signals so removing a source in OBS's
	   Sources list clears the rail badge immediately. The callbacks may arrive
	   from a non-Qt thread, so they hop onto the dock's thread. */
	obsSignals_ = obs_get_signal_handler();
	for (const char *signal : {"source_create", "source_destroy",
				   "source_rename"})
		signal_handler_connect(obsSignals_, signal,
				       &ZcControlDock::onObsSourceSignal, this);

	/* mDNS discovery: refresh the camera rail periodically. Runs on the Qt main
	   thread (the OBS frontend thread), which is also where the source is added,
	   so scene access is safe. */
	scanTimer_ = new QTimer(this);
	scanTimer_->setInterval(2500);
	connect(scanTimer_, &QTimer::timeout, this, &ZcControlDock::refreshCameras);
	scanTimer_->start();
	/* A closed dock does not need a discovery poll every 2.5 s: the rail is
	   not on screen, and the mDNS/source churn is pure overhead until the
	   dock is shown again. */
	connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
		if (!scanTimer_)
			return;
		if (visible)
			scanTimer_->start();
		else
			scanTimer_->stop();
	});

	/* A setting the camera applies by restarting its web service (HTTPS,
	   identity auth) or by moving to another address (network type) drops the
	   control connection until it comes back (bug 8). */
	connect(settings_, &ZcSettingsPanel::cameraRestarting, this,
		&ZcControlDock::onCameraRestarting);
	connect(settings_, &ZcSettingsPanel::loginRequested, this,
		&ZcControlDock::onLoginSubmitted);
	restartTimer_ = new QTimer(this);
	restartTimer_->setInterval(4000);
	connect(restartTimer_, &QTimer::timeout, this, &ZcControlDock::tryReconnect);
}

/* Query the mDNS discovery records for the currently advertised cameras and
   hand them to the rail, together with the cameras OBS holds as sources. The
   iterator holds the discovery lock for the duration of the loop only, so UI
   work afterwards (rebuilding the list, adding a source) never runs while
   records are being mutated. */
void ZcControlDock::refreshCameras()
{
	QList<QPair<QString, QString>> found;
	{
		SspMDnsIterator it;
		while (it.hasNext()) {
			ssp_device_item *d = it.next();
			if (!d || d->ip_address.empty())
				continue;
			found.append(qMakePair(
				QString::fromStdString(d->device_name),
				QString::fromStdString(d->ip_address)));
		}
	}

	QVector<ZcRailCamera> rows;
	QSet<QString> discovered;
	for (const auto &f : found) {
		if (f.second.isEmpty() || discovered.contains(f.second))
			continue;
		discovered.insert(f.second);
		rows.append({f.first, f.second,
			     unreachableHosts_.contains(f.second)});
	}

	/* Spec §1, L11: a camera OBS holds as a source is listed even when discovery
	   is not advertising it — otherwise a configured camera whose mDNS record is
	   missing could not be selected at all. It is marked offline until it answers
	   a control connection, and cannot be operated. Sorted by address so the rows
	   this adds do not move around between scans. */
	QList<QString> extraHosts;
	for (const QString &host : sourceHosts()) {
		if (!discovered.contains(host))
			extraHosts.append(host);
	}
	std::sort(extraHosts.begin(), extraHosts.end());
	for (const QString &host : extraHosts)
		rows.append({QString(), host,
			     unreachableHosts_.contains(host) ||
				 !reachableHosts_.contains(host)});

	if (rail_)
		rail_->setCameras(rows);
	/* Discovery and presence are independent (spec §1, L1/L4): a camera can be
	   listed without being a source, and a source can be removed at any time. */
	refreshRailPresence();
}

/* The source name that represents a camera. It is the only identity used for
   the added/not-added decision (spec §1, L3). */
QString ZcControlDock::sourceNameForHost(const QString &host)
{
	return zc::cameraSourceName(host);
}

/* Enumerating the sources OBS holds is what makes the rail independent of
   discovery: a `ZCamera <ip>` source that no mDNS record mentions still yields a
   row. Names that are not `ZCamera <ip>` (scenes, other plugins' sources) are
   filtered out by hostFromSourceName. */
static bool collectSourceHost(void *data, obs_source_t *source)
{
	auto *hosts = static_cast<QSet<QString> *>(data);
	const QString host =
	    zc::hostFromSourceName(QString::fromUtf8(obs_source_get_name(source)));
	if (!host.isEmpty())
		hosts->insert(host);
	return true;
}

QSet<QString> ZcControlDock::sourceHosts()
{
	QSet<QString> hosts;
	obs_enum_sources(collectSourceHost, &hosts);
	return hosts;
}

/* Find a zcamera_source whose settings hold this camera's address. The rail's
   "added" state keys off the source name ("ZCamera <ip>"), but a source can be
   created from OBS's own Sources panel and only given the address in its
   properties, in which case its name is the default "ZCamera". Matching by the
   stored zc_source_ip makes that source count as "added" too and lets a second
   dock add reuse it instead of stacking a duplicate (bug 15). Returns the
   source addref'd, or null. */
struct ZcHostMatch {
	QString host;
	obs_source_t *found = nullptr;
};
static bool findSourceByHostProc(void *param, obs_source_t *src)
{
	auto *m = static_cast<ZcHostMatch *>(param);
	if (strcmp(obs_source_get_id(src), "zcamera_source") != 0)
		return true;
	obs_data_t *settings = obs_source_get_settings(src);
	const char *ip = settings ? obs_data_get_string(settings, "zc_source_ip")
				  : nullptr;
	const bool match = ip && m->host == QString::fromUtf8(ip);
	obs_data_release(settings);
	if (match) {
		m->found = obs_source_get_ref(src);
		return false;
	}
	return true;
}
static obs_source_t *findSourceByHost(const QString &host)
{
	ZcHostMatch m{host, nullptr};
	obs_enum_sources(findSourceByHostProc, &m);
	return m.found;
}

/* Live presence query against OBS, never a cached flag (spec §1, L4). */
bool ZcControlDock::isCameraAdded(const QString &host)
{
	if (host.isEmpty())
		return false;
	const QByteArray name = sourceNameForHost(host).toUtf8();
	obs_source_t *src = obs_get_source_by_name(name.constData());
	if (!src)
		src = findSourceByHost(host);
	if (!src)
		return false;
	obs_source_release(src);
	return true;
}

void ZcControlDock::refreshRailPresence()
{
	if (!rail_)
		return;
	QSet<QString> added;
	QListWidget *list = rail_->list();
	for (int i = 0; i < list->count(); ++i) {
		const QString host = list->item(i)->data(kHostRole).toString();
		if (isCameraAdded(host))
			added.insert(host);
	}
	rail_->setAddedHosts(added);
}

void ZcControlDock::onObsSourceSignal(void *data, calldata_t *)
{
	auto *dock = static_cast<ZcControlDock *>(data);
	if (!dock)
		return;
	/* Source signals can fire off the Qt thread; marshal the UI update. */
	QMetaObject::invokeMethod(
	    dock, [dock]() { dock->refreshRailPresence(); },
	    Qt::QueuedConnection);
}

/* A source added from the dock is "ZCamera <ip>", but OBS may already hold a
   single unconfigured default source created manually via its Sources panel —
   named by get_name ("ZCamera", or the older "ZCameraPlugin.SourceName") and
   never given an address. Repurpose that one for this camera (rename it to
   "ZCamera <ip>") instead of leaving a second, duplicate source behind. Returns
   the source addref'd, or null. */
static obs_source_t *reclaimDefaultSource(const std::string &newName)
{
	static const char *candidates[] = {"ZCamera", "ZCameraPlugin.SourceName"};
	for (const char *cand : candidates) {
		obs_source_t *src = obs_get_source_by_name(cand);
		if (!src)
			continue;
		if (strcmp(obs_source_get_id(src), "zcamera_source") != 0) {
			obs_source_release(src);
			continue;
		}
		obs_data_t *sd = obs_source_get_settings(src);
		const char *ip = sd ? obs_data_get_string(sd, "zc_source_ip") : nullptr;
		const bool configured = ip && *ip;
		obs_data_release(sd);
		if (configured) { /* this default is already pointing at a camera */
			obs_source_release(src);
			continue;
		}
		obs_source_set_name(src, newName.c_str());
		return src;
	}
	return nullptr;
}

/* Add the camera's source to the current scene (spec §1, L6/L7). Creates the
   source when it does not exist, and re-applies the address when it does — a
   self-heal for sources saved before get_defaults stopped clobbering
   create-time settings (they hold an empty IP and stay black forever). An
   unchanged IP is ignored by OBS, so a healthy stream is not restarted. Must
   run on the OBS frontend thread. */
void ZcControlDock::addCameraToSource(const QString &host)
{
	if (host.isEmpty())
		return;
	const std::string ip = host.toStdString();
	const std::string name = sourceNameForHost(host).toStdString();

	obs_source_t *src = obs_get_source_by_name(name.c_str());
	obs_data_t *s = obs_data_create();
	/* Same key the source's getproperties uses (PROP_SOURCE_IP). */
	obs_data_set_string(s, "zc_source_ip", ip.c_str());
	if (src) {
		obs_source_update(src, s);
	} else {
		/* A source may already carry this address under another name (e.g.
		   added via the source-IP box as the default "ZCamera"): reuse it and
		   give it the standard name so the rail's by-name check matches and no
		   duplicate is created (bug 15). */
		src = findSourceByHost(QString::fromStdString(ip));
		if (src) {
			obs_source_set_name(src, name.c_str());
			obs_source_update(src, s);
		} else {
			/* First try to reclaim an unconfigured default source rather than
			   stacking a second one. */
			src = reclaimDefaultSource(name);
			if (src)
				obs_source_update(src, s);
			else
				src = obs_source_create("zcamera_source", name.c_str(), s,
							nullptr);
		}
	}
	obs_data_release(s);

	if (src) {
		obs_source_t *sceneSrc = obs_frontend_get_current_scene();
		if (sceneSrc) {
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene &&
			    !obs_scene_find_source(scene, name.c_str()))
				obs_scene_add(scene, src);
			obs_source_release(sceneSrc);
		}
		/* OBS activates the source (and the plugin calls start()) once the scene
		   it is in starts rendering; no manual activation is needed here. */
		obs_source_release(src);
	}
}

/* Remove the "ZCamera <host>" source from OBS entirely (out of every scene).
   A camera that is no longer a source is gone from the rail's "added" state and
   can be re-added later. Runs on the OBS frontend thread. */
void ZcControlDock::removeCameraFromSource(const QString &host)
{
	if (host.isEmpty())
		return;
	const std::string name = sourceNameForHost(host).toStdString();

	obs_source_t *src = obs_get_source_by_name(name.c_str());
	/* A source created from OBS's own Sources panel keys off its stored address,
	   not its name (the rail counts it as added the same way), so removing it must
	   find it the same way rather than leaving its gold check stuck on (bug list
	   "the +/check toggle is out of sync with the source"). */
	if (!src)
		src = findSourceByHost(host);
	if (!src)
		return;
	obs_source_remove(src);  /* out of all scenes; OBS drops its own refs */
	obs_source_release(src); /* release our look-up ref */
}

ZcControlDock::~ZcControlDock()
{
	if (obsSignals_) {
		for (const char *signal : {"source_create", "source_destroy",
					   "source_rename"})
			signal_handler_disconnect(obsSignals_, signal,
						  &ZcControlDock::onObsSourceSignal,
						  this);
	}
#ifdef ZC_ENABLE_DEBUG_PANEL
	if (debugPanel_)
		delete debugPanel_;
#endif
	/* A connect probe runs a nested event loop inside client_->connect(); OBS can
	   tear the dock down from an event delivered in it, leaving the probe's frames
	   on the stack to resume when the loop unwinds. Deleting the client here would
	   free its transport under negotiateOrigin(); letting Qt free it as this
	   dock's child would do the same, so when a probe is in flight detach it off
	   the dock and leak it (the process is going down). The resume points check
	   g_controlDockClosing and bail out before touching a freed member. */
	g_controlDockClosing = true;
	if (client_) {
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		if (opening_)
			client_->setParent(nullptr); /* the probe is still using it */
		else
			delete client_;
		client_ = nullptr;
	}
}

/* Spec §3: the PTZ tab exists only for a camera that reports PTZ support, and
   the auto-framing tab only for one that reports auto framing
   (feature.autoFraming). The two capabilities are independent, so each tab is
   added/removed on its own gate and only when the camera changed. */
void ZcControlDock::updatePtzTab()
{
	if (!tabs_ || !ptzTab_)
		return;

	/* Remember what was on screen: removing a tab does not fall back cleanly,
	   so switch to Settings if the visible pane just went away. */
	QWidget *shown = tabs_->currentWidget();

	const bool wantPtz = client_ && client_->cameraSupportsPtz();
	const int ptzIndex = tabs_->indexOf(ptzTab_);
	if (wantPtz && ptzIndex < 0)
		tabs_->addTab(ptzTab_, ztr("ZCameraPlugin.Dock.Ptz"));
	else if (!wantPtz && ptzIndex >= 0)
		tabs_->removeTab(ptzIndex);

	/* The auto-framing pane is inserted into the tab bar for a camera that
	   reports auto framing (see kFramingPanelVisible). The gate lives here, not
	   at construction, so hiding it again is a one-line change. */
	if (framingTab_ && kFramingPanelVisible) {
		const bool wantFraming =
		    client_ && client_->cameraSupportsAutoFraming();
		const int framingIndex = tabs_->indexOf(framingTab_);
		if (wantFraming && framingIndex < 0)
			tabs_->addTab(framingTab_,
				      ztr("ZCameraPlugin.Dock.Framing"));
		else if (!wantFraming && framingIndex >= 0)
			tabs_->removeTab(framingIndex);
	}

	/* Never leave the pane on a tab that just went away. */
	if (shown && tabs_->indexOf(shown) < 0)
		tabs_->setCurrentWidget(settings_);
}

/* Connect the control client to `host`. Returns false when the camera does not
   answer; the panels are then left without a client, so nothing can be operated
   (spec §1, L11). */
bool ZcControlDock::openCamera(const QString &host)
{
	return openCamera(host, creds_.value(host));
}

bool ZcControlDock::openCamera(const QString &host, const ZcCredentials &creds)
{
	/* connect() answers synchronously but runs a nested event loop (the origin
	   probe), so an event delivered during it can re-enter this path — a queued
	   selectCamera() from the source Properties dialog, or a second click. A
	   re-entrant call would run `delete client_` in openCameraNow() while the
	   current client's connect() frame (and its transport) is still on the stack,
	   freeing the object mid-call: the crash is in negotiateOrigin, at the
	   base_url_ build. Never serve a second request while one is live; remember
	   the pick and serve it after the current attempt returns. */
	if (opening_)
		return false;

	opening_ = true;
	const bool ok = openCameraNow(host, creds);
	/* openCameraNow() ran a nested event loop; OBS may have torn the dock down
	   from an event delivered in it (see g_controlDockClosing). If so, do not
	   touch any member of the freed dock. */
	if (g_controlDockClosing)
		return false;
	opening_ = false;

	/* Serve a pick that arrived while we were connecting. Deferred so the
	   caller's stack (openCameraNow's frames are gone) unwinds first. */
	if (pendingClick_) {
		pendingClick_ = false;
		const QString next = pendingClickHost_;
		pendingClickHost_.clear();
		QTimer::singleShot(0, this, [this, next]() { onCameraClicked(next); });
	}
	return ok;
}

bool ZcControlDock::openCameraNow(const QString &host, const ZcCredentials &creds)
{
	if (client_) {
		/* Detach the panels before the client goes away: a panel holding a
		   pointer to a deleted client would write through it. */
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		delete client_;
		client_ = nullptr;
	}
	client_ = new ZcCameraClient(this);
	settings_->setClient(client_);
	ptz_->setClient(client_);
	presets_->setClient(client_);
	framing_->setClient(client_);
	/* The preset/trace transport is greyed while auto-framing drives the head.
	   That state belongs to the PREVIOUS camera, so clear it on every switch;
	   the first framingStatusChanged from the new camera greys it again if it
	   actually needs to be (bug review §4 UI: stuck-disabled panel). */
	presets_->setEnabled(true);
#ifdef ZC_ENABLE_DEBUG_PANEL
	/* Feed the debug panel from this client's transport (see ZcCameraClient). */
	if (debugPanel_)
		connect(client_, &ZcCameraClient::requestLogged, debugPanel_,
			&ZcDebugPanel::append);
#endif

	bool ok = client_->connect(host, 80, creds);
	/* connect() ran a nested event loop (the origin probe); OBS may have torn the
	   dock down from an event delivered in it (see g_controlDockClosing). Bail
	   before touching any member of the freed dock. */
	if (g_controlDockClosing)
		return false;
	if (!ok) {
		/* Distinguish "the camera wants a login we do not have" from "the
		   camera is not there", so the caller can ask for credentials
		   (bug 4) instead of reporting it offline. */
		lastAuthRequired_ = client_->authRequired();
		lastTlsUnavailable_ = client_->tlsUnavailable();
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		framing_->setClient(nullptr);
		delete client_;
		client_ = nullptr;
		/* No client: the PTZ tab must go, and every control is inert. */
		updatePtzTab();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return false;
	}
	lastAuthRequired_ = false;
	lastTlsUnavailable_ = false;

	statusBarLabel_->setText(ztr("ZCameraPlugin.Dock.Connected").arg(host));
	settings_->hidePrompts();
	client_->refreshStatus();
	settings_->refreshGroup();

	connect(client_, &ZcCameraClient::statusChanged, this,
		[this](const QJsonObject &) {
			if (client_)
				statusBarLabel_->setText(ztr("ZCameraPlugin.Dock.ConnectedMode").arg(currentHost_).arg(client_->mode()));
		});
	/* The camera's notification socket is a live heartbeat: when it drops (power
	   loss / network drop) the row must show offline without the operator
	   clicking or refreshing (bug 49), and when it comes back the row clears. The
	   WS layer auto-reconnects, so a blip marks offline briefly then online
	   again — which is what actually happened. */
	connect(client_, &ZcCameraClient::notifierStateChanged, this,
		[this](bool live) {
			if (!client_ || currentHost_.isEmpty())
				return;
			if (live) {
				reachableHosts_.insert(currentHost_);
				unreachableHosts_.remove(currentHost_);
			} else {
				reachableHosts_.remove(currentHost_);
				unreachableHosts_.insert(currentHost_);
			}
			if (rail_)
				rail_->setHostOffline(currentHost_, !live);
		});
	/* /info arrives asynchronously: the tab appears (or disappears) and the
	   preset list loads once the model is known. */
	connect(client_, &ZcCameraClient::cameraInfoChanged, this, [this]() {
		updatePtzTab();
		if (client_ && client_->cameraSupportsPtz()) {
			presets_->refresh();
			/* Seed the position read-out and the range-limit sliders from
			   the camera (bug list: the Range Limit group). */
			ptz_->refreshState();
			/* The PTZ catalog carries the keys the PTZ tab and the preset pane
			   depend on (`ptz_limit` for the Range Limit group, `ptz_speed_mode`
			   for the per-preset speed row); read it once so those dependencies
			   are known without the operator having to open the group. */
			client_->readGroup(QLatin1String("ptz"), nullptr);
		}
		if (client_ && client_->cameraSupportsAutoFraming())
			framing_->refresh();
	});
	/* Live position (ptzfChange): the read-out follows the head as it moves. */
	connect(client_, &ZcCameraClient::ptzPositionChanged, this,
		[this](double pan, double tilt, double) {
			if (ptz_)
				ptz_->setPosition(pan, tilt);
		});
	/* Live framing notifications: the status event carries the switches, so the
	   panel repaints straight from it; a settings change just means re-read. */
	connect(client_, &ZcCameraClient::framingStatusChanged, this,
		[this](const QJsonObject &status) {
			if (framing_)
				framing_->applyStatus(status);
			/* While auto-framing drives the head, the preset/trace transport
			   does nothing, so grey it out instead of letting a click look
			   broken; restore it the moment framing turns off (bug 47). */
			if (presets_)
				presets_->setEnabled(!framingActive(status));
		});
	connect(client_, &ZcCameraClient::framingSettingsChanged, this,
		[this]() {
			if (framing_)
				framing_->refresh();
		});
	/* The WS event only reports framing CHANGES. If the camera was already
	   framing when we connected, the transport must be greyed now — waiting
	   for the next event would leave the panel claiming to work while every
	   preset/trace click is ignored. One snapshot read settles it. */
	if (client_->cameraSupportsAutoFraming())
		client_->framingQuery([this](const QJsonObject &status) {
			if (presets_)
				presets_->setEnabled(!framingActive(status));
		});
	/* A camera change must not leave the previous camera's PTZ tab behind. */
	updatePtzTab();
	return true;
}

/* Reconnect schedule after a write that makes the camera restart its web service
   (HTTPS, identity auth, Crop Sensor). The camera is unreachable for tens of
   seconds, so the retry gap grows 20 s -> 40 s -> 90 s (RD feedback). */
static constexpr int kRestartBackoffMs[] = {20000, 40000, 90000};
static constexpr int kRestartBackoffCount =
    sizeof(kRestartBackoffMs) / sizeof(kRestartBackoffMs[0]);

/* Bug 8: a write that makes the camera restart its web service (HTTPS,
   identity auth) or move to another address (network type) is applied by a
   camera that is briefly unreachable. Say so, then poll for it rather than
   leaving the pane dead. */
void ZcControlDock::onCameraRestarting()
{
	if (currentHost_.isEmpty() || !restartTimer_)
		return;
	statusBarLabel_->setText(
	    ztr("ZCameraPlugin.Dock.CameraRestarting").arg(currentHost_));
	restartAttempts_ = 0;
	/* A restarting camera is offline for tens of seconds, so the first probe
	   waits rather than hammering it (RD feedback: back off ~20 s, 40 s, 90 s). */
	restartTimer_->setInterval(kRestartBackoffMs[0]);
	restartTimer_->start();
}

void ZcControlDock::tryReconnect()
{
	const QString host = currentHost_;
	if (host.isEmpty()) {
		restartTimer_->stop();
		return;
	}
	/* A bounded number of tries: a camera that never comes back, or comes back
	   on another address, must not leave a timer polling for ever. */
	if (++restartAttempts_ > 8) {
		restartTimer_->stop();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return;
	}
	const bool reconnected = openCamera(host);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	if (reconnected) {
		restartTimer_->stop();
		reachableHosts_.insert(host);
		unreachableHosts_.remove(host);
		refreshRailPresence();
		return;
	}
	/* The restart applied a login (identity authentication): ask for it rather
	   than polling for a camera that will keep answering 401 (bug 4). */
	if (lastAuthRequired_) {
		restartTimer_->stop();
		settings_->showLoginPrompt(host);
		return;
	}
	/* HTTPS-only camera with no TLS backend: point the operator to http, in the
	   pane. */
	if (lastTlsUnavailable_) {
		restartTimer_->stop();
		settings_->showTlsPrompt(host);
		return;
	}
	/* Still down: widen the gap for the next probe (20 s, 40 s, then 90 s). */
	restartTimer_->setInterval(
	    kRestartBackoffMs[qMin(restartAttempts_, kRestartBackoffCount - 1)]);
	/* openCamera() reported the failed attempt; keep the restarting notice up
	   while the camera is still coming back. */
	statusBarLabel_->setText(
	    ztr("ZCameraPlugin.Dock.CameraRestarting").arg(host));
}

/* The in-page login form was submitted: keep the credentials for this session
   and reconnect so the settings pane appears (bug 4). */
void ZcControlDock::onLoginSubmitted(const QString &user, const QString &password)
{
	if (currentHost_.isEmpty())
		return;
	ZcCredentials creds;
	creds.username = user;
	creds.password = password;
	creds_[currentHost_] = creds;
	/* Mirror into the shared store so the source's Properties bitrate push
	   can authenticate to this camera too — the source has no login UI of
	   its own and reads credentials back from the store (memory-only). */
	zc::rememberCredentials(currentHost_, creds);
	/* The form can be submitted while a connect is still probing (the probe runs
	   a nested event loop). openCamera() would refuse and this handler would then
	   wrongly report the password as rejected, so defer the retry instead. */
	if (opening_) {
		pendingClick_ = true;
		pendingClickHost_ = currentHost_;
		return;
	}
	const bool ok = openCamera(currentHost_, creds);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	if (ok) {
		reachableHosts_.insert(currentHost_);
		unreachableHosts_.remove(currentHost_);
	} else if (lastAuthRequired_) {
		/* The camera still challenged us: the credentials are wrong. Keep the
		   form up and say the login was rejected, so the click does not look
		   like it did nothing (bug list "wrong credentials not validated").
		   Only claim rejection when the camera actually challenged: a network
		   timeout here is not a wrong password, and telling the operator the
		   password was rejected sends them chasing the wrong thing. */
		settings_->showLoginError();
	}
	refreshRailPresence();
}

/* Select the camera an OBS source represents. The source's Properties dialog
   opens the dock for that camera, so it must follow the source the operator
   clicked rather than leave the last one selected (bug 27). Selecting the same
   host again is a no-op: re-opening Properties must not reset the page or drop a
   half-entered login. */
void ZcControlDock::selectCamera(const QString &host)
{
	if (host.isEmpty())
		return;
	if (host == currentHost_ && client_)
		return;
	onCameraClicked(host);
}

/* Spec §1, L2: selecting a camera connects the control client only. Adding it
   to OBS is a separate, explicit action. Spec §1, L11: whether it answered is
   remembered, so a source-only row that is really there stays usable and one
   that is not stays marked offline. */
void ZcControlDock::onCameraClicked(const QString &host)
{
	/* An explicit pick ends any wait for a restarting camera — before the
	   deferral below too, otherwise the previous camera's pending restart probe
	   fires after the new camera connects and tears its connection down. Clearing
	   the attempt count makes a later restart start its backoff afresh. */
	if (restartTimer_)
		restartTimer_->stop();
	restartAttempts_ = 0;

	/* A connect is still in flight (its origin probe runs a nested event loop, so
	   this handler can be re-entered from an event it delivers). openCamera()
	   cannot be served now without freeing the live client, so remember the pick
	   and let openCamera() replay it once the current attempt returns. Skip the
	   offline bookkeeping: nothing was attempted for this host yet. */
	if (opening_) {
		qWarning().noquote()
			<< "[obs-zcamera] click deferred, connect in progress" << host;
		currentHost_ = host;
		pendingClick_ = true;
		pendingClickHost_ = host;
		return;
	}
	currentHost_ = host;
	bool ok = openCamera(host);
	/* The probe may have torn the dock down (see g_controlDockClosing): stop
	   before touching a freed member. */
	if (g_controlDockClosing)
		return;
	qWarning().noquote() << "[obs-zcamera] click" << host << "ok" << ok
			     << "authReq" << lastAuthRequired_
			     << "tlsUnavail" << lastTlsUnavailable_;
	/* The camera answered 401: it wants a login we do not have. Show the in-page
	   login form instead of a modal dialog. */
	if (!ok && lastAuthRequired_)
		settings_->showLoginPrompt(host);
	/* HTTPS-only camera with no TLS backend: show the switch-to-http notice in
	   the pane. */
	else if (!ok && lastTlsUnavailable_)
		settings_->showTlsPrompt(host);
	if (ok) {
		reachableHosts_.insert(host);
		unreachableHosts_.remove(host);
	} else {
		reachableHosts_.remove(host);
		unreachableHosts_.insert(host);
	}
	if (rail_)
		rail_->setHostOffline(host, !ok);
	refreshRailPresence();
}

} // namespace zc