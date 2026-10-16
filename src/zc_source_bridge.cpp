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

} /* namespace zc */
