// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs-auto-resize-output
// Per-scene recording / output settings for OBS Studio.
//
// On scene change the active scene's preset (stored in the scene source's
// private_settings) is applied: output/base resolution & FPS via
// obs_reset_video(), and recording folder/format/audio-tracks via the profile
// config for the next recording. Presets travel with a scene when it is
// duplicated, because libobs copies private_settings during duplication.

#include "PresetDock.hpp"
#include "ApplyPreset.hpp"
#include "plugin-log.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMetaObject>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-auto-resize-output", "en-US")

namespace {

constexpr const char *kDockId = "auto_resize_output_dock";

PresetDock *g_dock = nullptr;

void apply_current_scene()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (scene) {
		aro_apply_preset_for_scene(scene);
		obs_source_release(scene);
	}
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// Apply the startup scene once the UI/video pipeline is ready.
		apply_current_scene();
		if (g_dock)
			g_dock->refreshSceneList();
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		apply_current_scene();
		if (g_dock)
			g_dock->onProgramSceneChanged();
		break;

	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		// Completes a pending "restart recording to apply" sequence.
		aro_on_recording_stopped();
		break;

	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g_dock)
			g_dock->refreshSceneList();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		// Frontend is going away; drop our event hook early.
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		break;

	default:
		break;
	}
}

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Per-scene recording and output settings (resolution, FPS, "
	       "recording folder/format/audio tracks).";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Auto Resize Output";
}

bool obs_module_load(void)
{
	ARO_LOG(LOG_INFO, "loaded (version " PLUGIN_VERSION ")");

	// Create the dock. OBS reparents the widget into a dock and owns it
	// thereafter; obs_frontend_remove_dock() destroys it on unload.
	g_dock = new PresetDock();
	obs_frontend_add_dock_by_id(kDockId, "Auto Resize Output", g_dock);

	// Route apply-status messages to the dock (marshalled to the UI thread).
	aro_set_status_reporter([](const std::string &msg) {
		if (!g_dock)
			return;
		const QString qmsg = QString::fromStdString(msg);
		QMetaObject::invokeMethod(
			g_dock,
			[qmsg]() {
				if (g_dock)
					g_dock->showStatus(qmsg);
			},
			Qt::QueuedConnection);
	});

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	aro_set_status_reporter(nullptr);
	aro_shutdown();

	obs_frontend_remove_dock(kDockId); // destroys the dock widget
	g_dock = nullptr;

	ARO_LOG(LOG_INFO, "unloaded");
}
