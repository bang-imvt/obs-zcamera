/*
 * obs-zcamera — implementation of the source <-> dock interaction bridge.
 * See zc_source_bridge.h for why the two sides meet here.
 *
 * Everything in this file is touched from the OBS UI thread: the source
 * registers from update()/its destructor, the dock pushes from its own
 * widgets, and the Interact window's mouse callbacks run on the UI thread
 * too. The mutex therefore only guards against a source being destroyed
 * while a push is in flight, which is cheap insurance for a table that is
 * read and written a handful of times per session.
 */
#include "zc_source_bridge.h"

#include <QHash>
#include <QMutex>

#include <cmath>

namespace zc {
namespace {

QMutex g_mutex;
QHash<QString, OverlayTarget *> g_targets;
OverlayPointerSink *g_sink = nullptr;

} /* namespace */

void overlaySetPointerSink(OverlayPointerSink *sink)
{
	QMutexLocker lock(&g_mutex);
	g_sink = sink;
}

void overlayDispatchPointer(int phase, int button, float nx, float ny)
{
	OverlayPointerSink *sink = nullptr;
	{
		QMutexLocker lock(&g_mutex);
		sink = g_sink;
	}
	if (sink)
		sink->onOverlayPointer(phase, button, nx, ny);
}

void overlayRegisterTarget(const QString &ip, OverlayTarget *target)
{
	if (ip.isEmpty() || !target)
		return;
	QMutexLocker lock(&g_mutex);
	g_targets.insert(ip, target);
}

void overlayUnregisterTarget(const QString &ip, OverlayTarget *target)
{
	if (ip.isEmpty())
		return;
	QMutexLocker lock(&g_mutex);
	/* Only drop the entry if it is still this object: a source that changed
	   its address and re-registered under the new one must not have its
	   successor's entry removed by a late teardown. */
	auto it = g_targets.find(ip);
	if (it != g_targets.end() && it.value() == target)
		g_targets.erase(it);
}

OverlayTarget *overlayFindTarget(const QString &ip)
{
	if (ip.isEmpty())
		return nullptr;
	QMutexLocker lock(&g_mutex);
	auto it = g_targets.find(ip);
	return it == g_targets.end() ? nullptr : it.value();
}

namespace {
float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
} /* namespace */

MaskGesture maskGestureAt(float left, float top, float right, float bottom,
			  bool on, float nx, float ny)
{
	if (!on || right - left < 1e-4f || bottom - top < 1e-4f)
		return MaskGesture::New;

	const float tol = kMaskGrabTolerance;
	const bool nearL = std::fabs(nx - left) <= tol;
	const bool nearR = std::fabs(nx - right) <= tol;
	const bool nearT = std::fabs(ny - top) <= tol;
	const bool nearB = std::fabs(ny - bottom) <= tol;

	if (nearL && nearT)
		return MaskGesture::ResizeLeftTop;
	if (nearR && nearT)
		return MaskGesture::ResizeRightTop;
	if (nearL && nearB)
		return MaskGesture::ResizeLeftBottom;
	if (nearR && nearB)
		return MaskGesture::ResizeRightBottom;
	if (nx > left && nx < right && ny > top && ny < bottom)
		return MaskGesture::Move;
	return MaskGesture::New;
}

void maskDragRect(MaskGesture gesture, float baseL, float baseT, float baseR,
		  float baseB, float grabX, float grabY, float nx, float ny,
		  float *outL, float *outT, float *outR, float *outB)
{
	float l = baseL, t = baseT, r = baseR, b = baseB;
	switch (gesture) {
	case MaskGesture::New:
		l = qMin(grabX, nx);
		r = qMax(grabX, nx);
		t = qMin(grabY, ny);
		b = qMax(grabY, ny);
		break;
	case MaskGesture::Move: {
		/* The pointer keeps its grip on the rectangle: the whole thing
		   travels by the delta, clamped so it cannot leave the picture. */
		const float w = baseR - baseL, h = baseB - baseT;
		float dx = nx - grabX, dy = ny - grabY;
		dx = qBound(-baseL, dx, 1.0f - baseR);
		dy = qBound(-baseT, dy, 1.0f - baseB);
		l = baseL + dx;
		r = l + w;
		t = baseT + dy;
		b = t + h;
		break;
	}
	case MaskGesture::ResizeLeftTop:
		l = qMin(nx, baseR);
		t = qMin(ny, baseB);
		r = baseR;
		b = baseB;
		break;
	case MaskGesture::ResizeRightTop:
		l = baseL;
		t = qMin(ny, baseB);
		r = qMax(nx, baseL);
		b = baseB;
		break;
	case MaskGesture::ResizeLeftBottom:
		l = qMin(nx, baseR);
		t = baseT;
		r = baseR;
		b = qMax(ny, baseT);
		break;
	case MaskGesture::ResizeRightBottom:
		l = baseL;
		t = baseT;
		r = qMax(nx, baseL);
		b = qMax(ny, baseT);
		break;
	case MaskGesture::None:
	default:
		break;
	}
	if (outL)
		*outL = clamp01(l);
	if (outT)
		*outT = clamp01(t);
	if (outR)
		*outR = clamp01(r);
	if (outB)
		*outB = clamp01(b);
}

} /* namespace zc */
