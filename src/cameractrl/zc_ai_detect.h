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

/* The camera's AI detection payload, as a typed value. Split out of the
   notifier so the decoding can be unit-tested without a camera on the wire
   (tests/cameractrl_tests.cpp), exactly like the schema helpers. */

#pragma once

#include <QJsonObject>
#include <QVector>

namespace zc {

/* One object from the camera's `AI` notification (ws_events::kAiDetect). The
   camera reports everything its AI sees — people, heads, faces, and the
   debug/lost variants — as one flat record per object.

   x/y/width/height are NORMALIZED to the video frame (0..1, top-left corner
   plus size). That is how the camera sends them and how its own web client
   uses them: it multiplies by the canvas width/height right before drawing
   (`x = Math.round(objs[k] * canvas.width)`), so the numbers are resolution
   and frame-rate independent. */
struct ZcAiBox {
	int app = 0;        /* which camera feature produced it (0..3)     */
	int id = 0;         /* tracking id, stable while the target lives  */
	int type = 0;       /* AI_ID_HUMAN / AI_ID_HEAD / AI_ID_FACE / ...  */
	int confidence = 0; /* < 0 means the object sits in a masked area   */
	double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
};

/* Decode one `AI` payload into its objects. The wire form is a flat array read
   `cols` at a time — [app, id, type, confidence, x, y, width, height] — with
   `num` records; the camera currently sends cols = 8. A short or malformed
   array yields only the records that are fully present, and an empty result
   (num == 0) simply means the camera sees nothing right now. */
QVector<ZcAiBox> parseAiDetection(const QJsonObject &payload);

} // namespace zc
