/*
obs-zcamera
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

#include "zc_ai_detect.h"

#include <QJsonArray>

namespace zc {

/* Field order is the camera's own: its web client walks the same array with
   `k = cols * i` and reads app, id, type, condident, x, y, width, height in
   that order (controller.min.js, AFFaceCtrl), and the firmware's `cols` field
   confirms the stride (measured on e2ptz sw 1.0.3:
   {"what":"AI","num":0,"cols":8,"sel":174,"objs":[]}).
   Reading past the end is guarded rather than trusted — firmware builds
   differ, and a truncated array must not read the next record's fields as
   this one's. */
QVector<ZcAiBox> parseAiDetection(const QJsonObject &payload)
{
	QVector<ZcAiBox> boxes;

	const int num = payload.value(QLatin1String("num")).toInt(0);
	const int cols = payload.value(QLatin1String("cols")).toInt(0);
	const QJsonArray objs = payload.value(QLatin1String("objs")).toArray();
	if (num <= 0 || cols <= 0 || objs.isEmpty())
		return boxes;

	/* The known record is 8 wide whatever `cols` says; taking the smaller
	   stride means a firmware that widens the record still decodes the part we
	   know instead of failing outright. */
	const int stride = cols < 8 ? cols : 8;
	for (int i = 0; i < num; i++) {
		const int k = cols * i;
		if (k + stride > objs.size())
			break;
		ZcAiBox b;
		b.app = objs.at(k).toInt();
		b.id = objs.at(k + 1).toInt();
		b.type = objs.at(k + 2).toInt();
		b.confidence = objs.at(k + 3).toInt();
		if (stride >= 8) {
			b.x = objs.at(k + 4).toDouble();
			b.y = objs.at(k + 5).toDouble();
			b.width = objs.at(k + 6).toDouble();
			b.height = objs.at(k + 7).toDouble();
		}
		boxes.append(b);
	}
	return boxes;
}

} // namespace zc
