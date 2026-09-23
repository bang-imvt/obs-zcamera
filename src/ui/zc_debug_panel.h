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

/* Sanitized request/event log for engineers. Never logs credentials. */

#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>

namespace zc {

class ZcDebugPanel : public QWidget {
	Q_OBJECT
public:
	explicit ZcDebugPanel(QWidget *parent = nullptr);

public slots:
	void append(const QString &line);

private slots:
	void clearLog();

private:
	QPlainTextEdit *log_ = nullptr;
	QPushButton *clearBtn_ = nullptr;
	QStringList recent_;
};

} // namespace zc