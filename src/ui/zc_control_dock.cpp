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
#include <QMessageBox>
#include <QInputDialog>
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
	   without an entry shows its raw id. */
	static const QHash<QString, const char *> kLabels = {
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
	};
	auto it = kLabels.constFind(key);
	if (it == kLabels.constEnd())
		return key;
	/* obs_module_text echoes the key itself when the locale has no entry. */
	const QString text = ztr(*it);
	return text == QLatin1String(*it) ? key : text;
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
	}
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
	auto *refresh = new QPushButton("Refresh", this);
	refresh->setToolTip("Re-read the stream settings and this group from the "
			    "camera");
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
		auto *item = new QListWidgetItem(cat.id, subtabList_);
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

	auto addHint = [&](const QString &text) {
		auto *hint = new QLabel(text, rowsContainer_);
		hint->setWordWrap(true);
		hint->setStyleSheet("color:#8a9bb2;padding:8px;");
		rowLayout->addWidget(hint);
	};

	if (catalog.isEmpty()) {
		addHint("Select a group to load its settings.");
		return;
	}
	if (!client_) {
		addHint("Select a camera to load its settings.");
		return;
	}
	if (!client_->isConnected()) {
		addHint("Not connected to a camera.");
		return;
	}

	/* Render the keys the camera reported for this catalog, in its order.
	   An empty list means the group was not read yet, or the camera does not
	   have this catalog (getbatch answers code -1 with no cfgs). */
	const QStringList keys = client_->settingGroupKeys(catalog);
	if (keys.isEmpty()) {
		addHint("No settings available for this group.");
		return;
	}

	for (const QString &key : keys) {
		ZcSettingValue def;
		if (!client_->settingDefinition(key, &def))
			continue;
		/* Unsupported (code -1) keys are hidden; read-only (ro=1) keys are shown
		   but disabled (greyed) so the operator can see the value without being
		   able to change it. */
		if (!def.supported)
			continue;
		const bool readOnly = def.readOnly;

		auto *row = new QHBoxLayout;
		auto *label = new QLabel(settingLabel(key), rowsContainer_);
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
			control = combo;
		} else if (def.type == 2 && def.max > def.min && def.step > 0 &&
			   def.step == std::floor(def.step)) {
			/* Range -> slider + live gold value label. */
			auto *wrap = new QWidget(rowsContainer_);
			auto *wl = new QHBoxLayout(wrap);
			wl->setContentsMargins(0, 0, 0, 0);
			auto *slider = new QSlider(Qt::Horizontal, wrap);
			slider->setMinimum(static_cast<int>(def.min));
			slider->setMaximum(static_cast<int>(def.max));
			slider->setSingleStep(static_cast<int>(def.step));
			slider->setValue(def.value.toInt());
			auto *val = new QLabel(def.value, wrap);
			val->setMinimumWidth(48);
			val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			val->setStyleSheet("color:#d8b878;");
			connect(slider, &QSlider::valueChanged, val,
				[val](int v) { val->setText(QString::number(v)); });
			connect(slider, &QSlider::sliderReleased, this,
				[this, key, slider] {
					writeKey(key,
						 QString::number(slider->value()));
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
	}
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
	   value is always kept representable. */
	const int bitrate = streamDoc_.value("bitrate").toInt();
	streamBitrate_->setRange(0, qMax(streamDoc_.value("maxBitrate").toInt(),
					 bitrate));
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
	fillChoice(streamCodec_,
		   jsonStringList(streamDoc_.value("videoEncoderList")),
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
	auto *speedLbl = new QLabel("Speed", this);
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

	layout->addStretch();
}

void ZcPtzPad::setClient(ZcCameraClient *client)
{
	client_ = client;
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
	int speed = speedSlider_->value();
	int pan = kPanDelta[dirIndex] * speed;
	int tilt = kTiltDelta[dirIndex] * speed;
	if (client_)
		client_->ptzMove(pan, tilt);
	emit moveRequested(pan, tilt);
}

void ZcPtzPad::moveAnalog(const QPointF &offset)
{
	int speed = speedSlider_->value();
	qreal px = offset.x() / joyOuterRadius_;
	qreal py = offset.y() / joyOuterRadius_;
	px = std::max<qreal>(-1.0, std::min<qreal>(1.0, px));
	py = std::max<qreal>(-1.0, std::min<qreal>(1.0, py));
	int pan = qRound(px * speed);
	int tilt = qRound(py * speed);
	if (client_)
		client_->ptzMove(pan, tilt);
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
	   reply carries no status. */
	if (!trace && info.value("status").toInt(0) != 0)
		hintLabel_->setText(ztr("ZCameraPlugin.Dock.PresetBusy"));

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
		primaryBtn_->setEnabled(has);
		/* "Add" writes a new preset, "Replace" overwrites a named one — the
		   same label toggle the camera's web UI uses. */
		secondaryBtn_->setText(named ? ztr("ZCameraPlugin.Dock.Replace") : ztr("ZCameraPlugin.Dock.Add"));
		secondaryBtn_->setEnabled(has);
		deleteBtn_->setEnabled(has);
		renameBtn_->setEnabled(has);
		unitCombo_->setEnabled(has);
		speedCombo_->setEnabled(has && speedCombo_->count() > 0);
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

	/* 1..18 characters; anything else reverts to the stored name. */
	if (name.isEmpty() || name.size() > kPresetNameMax) {
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

ZcControlDock::ZcControlDock(QWidget *parent) : QDockWidget(parent)
{
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
	ptz_ = new ZcPtzPad(ptzTab_);
	presets_ = new ZcPresetPanel(ptzTab_);
	ptzLayout->addWidget(ptz_, 0);
	ptzLayout->addWidget(presets_, 1);
	rightLayout->addWidget(tabs_, 1);

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
	debugButton_ = new QPushButton("Debug", container);
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

	/* A setting the camera applies by restarting its web service (HTTPS,
	   identity auth) or by moving to another address (network type) drops the
	   control connection until it comes back (bug 8). */
	connect(settings_, &ZcSettingsPanel::cameraRestarting, this,
		&ZcControlDock::onCameraRestarting);
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

/* Live presence query against OBS, never a cached flag (spec §1, L4). */
bool ZcControlDock::isCameraAdded(const QString &host)
{
	if (host.isEmpty())
		return false;
	const QByteArray name = sourceNameForHost(host).toUtf8();
	obs_source_t *src = obs_get_source_by_name(name.constData());
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
		/* First try to reclaim an unconfigured default source rather than
		   stacking a second one. */
		src = reclaimDefaultSource(name);
		if (src)
			obs_source_update(src, s);
		else
			src = obs_source_create("zcamera_source", name.c_str(), s,
						nullptr);
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
	delete client_;
}

/* Spec §3: the PTZ tab exists only for a camera that reports PTZ support. */
void ZcControlDock::updatePtzTab()
{
	if (!tabs_ || !ptzTab_)
		return;
	const bool want = client_ && client_->cameraSupportsPtz();
	const int index = tabs_->indexOf(ptzTab_);
	if (want && index < 0) {
		tabs_->addTab(ptzTab_, ztr("ZCameraPlugin.Dock.Ptz"));
	} else if (!want && index >= 0) {
		/* removeTab keeps the widget alive; it is re-inserted on the next
		   PTZ-capable camera. */
		tabs_->removeTab(index);
		tabs_->setCurrentWidget(settings_);
	}
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
	if (client_) {
		/* Detach the panels before the client goes away: a panel holding a
		   pointer to a deleted client would write through it. */
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		delete client_;
		client_ = nullptr;
	}
	client_ = new ZcCameraClient(this);
	settings_->setClient(client_);
	ptz_->setClient(client_);
	presets_->setClient(client_);

	bool ok = client_->connect(host, 80, creds);
	if (!ok) {
		/* Distinguish "the camera wants a login we do not have" from "the
		   camera is not there", so the caller can ask for credentials
		   (bug 4) instead of reporting it offline. */
		lastAuthRequired_ = client_->authRequired();
		settings_->setClient(nullptr);
		ptz_->setClient(nullptr);
		presets_->setClient(nullptr);
		delete client_;
		client_ = nullptr;
		/* No client: the PTZ tab must go, and every control is inert. */
		updatePtzTab();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return false;
	}
	lastAuthRequired_ = false;

	statusBarLabel_->setText(ztr("ZCameraPlugin.Dock.Connected").arg(host));
	client_->refreshStatus();
	settings_->refreshGroup();

	connect(client_, &ZcCameraClient::statusChanged, this,
		[this](const QJsonObject &) {
			if (client_)
				statusBarLabel_->setText(ztr("ZCameraPlugin.Dock.ConnectedMode").arg(currentHost_).arg(client_->mode()));
		});
	/* /info arrives asynchronously: the tab appears (or disappears) and the
	   preset list loads once the model is known. */
	connect(client_, &ZcCameraClient::cameraInfoChanged, this, [this]() {
		updatePtzTab();
		if (client_ && client_->cameraSupportsPtz())
			presets_->refresh();
	});
	/* A camera change must not leave the previous camera's PTZ tab behind. */
	updatePtzTab();
	return true;
}

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
	if (++restartAttempts_ > 15) {
		restartTimer_->stop();
		statusBarLabel_->setText(
		    ztr("ZCameraPlugin.Dock.OfflineReason").arg(host));
		return;
	}
	if (openCamera(host)) {
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
		if (promptForCredentials(host)) {
			reachableHosts_.insert(host);
			unreachableHosts_.remove(host);
			refreshRailPresence();
		}
		return;
	}
	/* openCamera() reported the failed attempt; keep the restarting notice up
	   while the camera is still coming back. */
	statusBarLabel_->setText(
	    ztr("ZCameraPlugin.Dock.CameraRestarting").arg(host));
}

/* Bug 4: the camera's control API is behind a login (identity authentication
   was switched on, or HTTPS with auth). Ask for the login the operator set on
   the camera and retry, so the pane is usable again instead of showing empty
   settings. The credentials are kept for this session only. */
bool ZcControlDock::promptForCredentials(const QString &host)
{
	bool accepted = false;
	const QString user = QInputDialog::getText(
	    this, ztr("ZCameraPlugin.Dock.AuthTitle"),
	    ztr("ZCameraPlugin.Dock.AuthUser").arg(host), QLineEdit::Normal,
	    QString(), &accepted);
	if (!accepted || user.isEmpty())
		return false;
	const QString pass = QInputDialog::getText(
	    this, ztr("ZCameraPlugin.Dock.AuthTitle"),
	    ztr("ZCameraPlugin.Dock.AuthPassword"), QLineEdit::Password,
	    QString(), &accepted);
	if (!accepted)
		return false;

	ZcCredentials creds;
	creds.username = user;
	creds.password = pass;
	if (!openCamera(host, creds))
		return false;
	creds_.insert(host, creds);
	return true;
}

/* Spec §1, L2: selecting a camera connects the control client only. Adding it
   to OBS is a separate, explicit action. Spec §1, L11: whether it answered is
   remembered, so a source-only row that is really there stays usable and one
   that is not stays marked offline. */
void ZcControlDock::onCameraClicked(const QString &host)
{
	/* An explicit pick ends any wait for a restarting camera. */
	if (restartTimer_)
		restartTimer_->stop();
	currentHost_ = host;
	bool ok = openCamera(host);
	/* The camera answered 401: it wants a login we do not have yet (bug 4). */
	if (!ok && lastAuthRequired_)
		ok = promptForCredentials(host);
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