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

#include "zc_debug_panel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QRegularExpression>

namespace zc {

ZcDebugPanel::ZcDebugPanel(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	log_ = new QPlainTextEdit(this);
	log_->setReadOnly(true);
	log_->setMaximumBlockCount(2000); /* bounded memory */

	auto *btnRow = new QHBoxLayout;
	clearBtn_ = new QPushButton("Clear", this);
	connect(clearBtn_, &QPushButton::clicked, this, &ZcDebugPanel::clearLog);
	btnRow->addStretch();
	btnRow->addWidget(clearBtn_);

	layout->addWidget(log_, 1);
	layout->addLayout(btnRow);
}

void ZcDebugPanel::append(const QString &line)
{
	/* Sanitize: strip Authorization/cookie/credential-like values. */
	QString safe = line;
	safe.replace(QRegularExpression(
			    "(Authorization|X-Auth-Token|Cookie|Password|password)\\s*[:=]\\s*\\S+"),
		     "\\1: [redacted]");
	log_->appendPlainText(QDateTime::currentDateTime()
				      .toString("HH:mm:ss.zzz ") + safe);
	recent_.append(safe);
	if (recent_.size() > 2000)
		recent_.removeFirst();
}

void ZcDebugPanel::clearLog()
{
	log_->clear();
	recent_.clear();
}

} // namespace zc