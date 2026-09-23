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

/* Clean plugin entry point for obs-zcamera.

   Registers the SSP source and the camera-control dock. Kept from the
   reference: mDNS discovery and the ssp-connector subprocess. Dropped: the
   obs-browser (CEF) toolbar, the device-list dock and the monolithic source. */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QAction>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>

#include "obs-ssp.h"
#include "ssp-mdns.h"
#include "ui/zc_control_dock.h"

// Minimum OBS version required for this plugin (from buildspec.json)
#define MIN_OBS_VERSION_MAJOR 31
#define MIN_OBS_VERSION_MINOR 0
#define MIN_OBS_VERSION_PATCH 0

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("IMVT")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-zcamera", "en-US")

extern struct obs_source_info create_ssp_source_info();
struct obs_source_info ssp_source_info;

using zc::ZcControlDock;

/* QPointer, not a raw pointer: the dock is parented to OBS's main window, so
   Qt destroys it as part of that window's teardown. Whichever happens first,
   the pointer must not be left dangling — deleting it twice corrupts the heap
   and fast-fails the CRT (0xC0000409) on exit. */
static QPointer<ZcControlDock> zc_control_dock;

static void show_zcamera_control_dock(void *, obs_data_t *)
{
	if (!zc_control_dock) {
		QMainWindow *main_window =
			(QMainWindow *)obs_frontend_get_main_window();
		zc_control_dock = new ZcControlDock(main_window);
		main_window->addDockWidget(Qt::RightDockWidgetArea,
					   zc_control_dock);
		zc_control_dock->setFloating(true);
	}
	zc_control_dock->show();
	zc_control_dock->raise();
	zc_control_dock->activateWindow();
}

/* Called by the source's properties dialog (get_properties), which can run off
   the Qt thread; marshal the show onto the OBS frontend (Qt) thread. */
void zc_show_control_dock()
{
	QMainWindow *main_window =
		(QMainWindow *)obs_frontend_get_main_window();
	if (!main_window)
		return;
	QMetaObject::invokeMethod(
		main_window, []() { show_zcamera_control_dock(nullptr, nullptr); },
		Qt::QueuedConnection);
}

/* Bring the control dock forward when a ZCamera source appears, so an operator
   who added one from OBS's own Sources panel lands on the surface that drives
   it. Sources are created just as much while a scene collection loads, and
   popping the dock open on every launch would be hostile, so the raise is armed
   only once the frontend has finished loading. */
static bool zc_dock_raise_armed = false;
static signal_handler_t *zc_obs_signals = nullptr;

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		zc_dock_raise_armed = true;
}

static void on_source_created(void *, calldata_t *params)
{
	if (!zc_dock_raise_armed)
		return;
	obs_source_t *source = nullptr;
	if (!calldata_get_ptr(params, "source", (void **)&source) || !source)
		return;
	if (strcmp(obs_source_get_id(source), "zcamera_source") != 0)
		return;
	QMainWindow *main_window =
		(QMainWindow *)obs_frontend_get_main_window();
	if (!main_window)
		return;
	/* The signal can arrive off the Qt thread; the dock is Qt. */
	QMetaObject::invokeMethod(
		main_window, []() { show_zcamera_control_dock(nullptr, nullptr); },
		Qt::QueuedConnection);
}

static bool check_obs_version_compatibility()
{
	uint32_t obs_version = obs_get_version();
	uint8_t obs_major = (obs_version >> 24) & 0xFF;
	uint8_t obs_minor = (obs_version >> 16) & 0xFF;

	ssp_blog(LOG_INFO, "OBS Studio version: %d.%d",
		 obs_major, obs_minor);

	if (obs_major < MIN_OBS_VERSION_MAJOR)
		return false;
	if (obs_major == MIN_OBS_VERSION_MAJOR &&
	    obs_minor < MIN_OBS_VERSION_MINOR)
		return false;
	return true;
}

bool obs_module_load(void)
{
	ssp_blog(LOG_INFO, "obs-zcamera %s loading", PLUGIN_VERSION);

	if (!check_obs_version_compatibility()) {
		QMainWindow *main_window =
			(QMainWindow *)obs_frontend_get_main_window();
		QMetaObject::invokeMethod(main_window, [main_window]() {
			QMessageBox::critical(
				main_window,
				obs_module_text("SSPPlugin.VersionCheck.Title"),
				obs_module_text("SSPPlugin.VersionCheck.Error"),
				QMessageBox::Ok);
		}, Qt::QueuedConnection);
		return false;
	}

	/* mDNS camera discovery. */
	create_mdns_loop();

	/* Register the SSP source. */
	ssp_source_info = create_ssp_source_info();
	obs_register_source(&ssp_source_info);

	/* Camera-control dock. */
	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("ZCameraPlugin.Menu.ShowControlDock"));
	QObject::connect(action, &QAction::triggered, [](bool) {
		show_zcamera_control_dock(nullptr, nullptr);
	});

	/* Bring the dock forward when a ZCamera source is created (see above). */
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	zc_obs_signals = obs_get_signal_handler();
	signal_handler_connect(zc_obs_signals, "source_create", on_source_created,
			       nullptr);

	return true;
}

void obs_module_unload(void)
{
	if (zc_obs_signals) {
		signal_handler_disconnect(zc_obs_signals, "source_create",
					  on_source_created, nullptr);
		zc_obs_signals = nullptr;
	}
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	stop_mdns_loop();
	/* Null already when Qt took the dock down with the main window. */
	if (zc_control_dock) {
		zc_control_dock->setParent(nullptr);
		delete zc_control_dock;
	}
	ssp_blog(LOG_INFO, "obs-zcamera unloaded");
}

const char *obs_module_name()
{
	return "obs-zcamera";
}

const char *obs_module_description()
{
	return "ZCAM (SSP) camera source with zero-copy hardware decode for OBS Studio";
}