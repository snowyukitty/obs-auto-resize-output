// SPDX-License-Identifier: GPL-2.0-or-later

#include "ApplyPreset.hpp"
#include "plugin-log.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <cstring>

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------

// Set when a video override is deferred until the current recording stops.
static obs_weak_source_t *g_pending_restart_scene = nullptr;

static StatusReporter g_status_reporter;

void aro_set_status_reporter(StatusReporter reporter)
{
	g_status_reporter = std::move(reporter);
}

static void report(const std::string &msg)
{
	if (g_status_reporter)
		g_status_reporter(msg);
}

// ---------------------------------------------------------------------------
// Recording config (profile config_t)
// ---------------------------------------------------------------------------

// OBS stores recording settings in different config sections depending on the
// chosen output mode. These keys target OBS 30+/31+ (RecFormat2 era).
static void apply_recording_config(const ScenePreset &p)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return;

	const char *mode = config_get_string(cfg, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *section = advanced ? "AdvOut" : "SimpleOutput";

	bool changed = false;

	if (p.use_rec_path && !p.rec_path.empty()) {
		const char *key = advanced ? "RecFilePath" : "FilePath";
		config_set_string(cfg, section, key, p.rec_path.c_str());
		changed = true;
	}

	if (p.use_rec_format && !p.rec_format.empty()) {
		config_set_string(cfg, section, "RecFormat2",
				  p.rec_format.c_str());
		changed = true;
	}

	if (p.use_audio_tracks) {
		// Bitmask of enabled recording tracks (bit0 = track 1).
		config_set_int(cfg, section, "RecTracks", p.audio_tracks);
		changed = true;
	}

	if (changed) {
		config_save(cfg);
		ARO_LOG(LOG_INFO,
			"Applied recording config (mode=%s) for upcoming recordings",
			advanced ? "Advanced" : "Simple");
	}
}

// Recording video bitrate. In Advanced output mode the recording encoder's
// settings live in <profile>/recordEncoder.json; OBS reads that file when it
// builds the recording output at "Start Recording". We set a CBR bitrate there
// so it applies to the next recording. Simple mode has no separate recording
// bitrate (it shares the stream encoder), so we skip it rather than mutate
// streaming settings behind the user's back.
static void apply_recording_bitrate(const ScenePreset &p)
{
	if (!p.use_rec_bitrate)
		return;

	config_t *cfg = obs_frontend_get_profile_config();
	const char *mode = cfg ? config_get_string(cfg, "Output", "Mode")
			       : nullptr;
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	if (!advanced) {
		ARO_LOG(LOG_WARNING,
			"Recording bitrate override needs Advanced output mode; ignored in Simple mode");
		return;
	}

	char *profile_path = obs_frontend_get_current_profile_path();
	if (!profile_path)
		return;
	const std::string path =
		std::string(profile_path) + "/recordEncoder.json";
	bfree(profile_path);

	// Preserve any existing encoder settings; only override rate control.
	obs_data_t *enc = obs_data_create_from_json_file(path.c_str());
	if (!enc)
		enc = obs_data_create();
	obs_data_set_string(enc, "rate_control", "CBR");
	obs_data_set_int(enc, "bitrate", p.rec_bitrate);
	obs_data_save_json_safe(enc, path.c_str(), "tmp", "bak");
	obs_data_release(enc);

	ARO_LOG(LOG_INFO,
		"Set recording encoder to CBR %u kbps for upcoming recordings",
		p.rec_bitrate);
}

// ---------------------------------------------------------------------------
// Video (obs_reset_video)
// ---------------------------------------------------------------------------

// Mirror applied video values into the profile config so the Settings dialog
// and the next launch reflect what is actually running.
static void persist_video_config(const ScenePreset &p,
				 const struct obs_video_info &ovi)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return;

	if (p.use_base_res) {
		config_set_uint(cfg, "Video", "BaseCX", ovi.base_width);
		config_set_uint(cfg, "Video", "BaseCY", ovi.base_height);
	}
	if (p.use_output_res) {
		config_set_uint(cfg, "Video", "OutputCX", ovi.output_width);
		config_set_uint(cfg, "Video", "OutputCY", ovi.output_height);
	}
	if (p.use_fps) {
		// FPSType 1 == "Integer FPS Value"; FPSInt holds the value.
		config_set_uint(cfg, "Video", "FPSType", 1);
		config_set_uint(cfg, "Video", "FPSInt", p.fps);
	}
	config_save(cfg);
}

static VideoApplyResult apply_video(const ScenePreset &p)
{
	if (!p.any_video_override())
		return VideoApplyResult::NoChange;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		// Video pipeline not initialized yet.
		return VideoApplyResult::Failed;
	}

	// obs_get_video_info copies the stored struct, which normally carries the
	// graphics_module pointer from first init. Guard against it being unset so
	// obs_reset_video can always (re)initialize graphics if it ever needs to.
	if (!ovi.graphics_module) {
#ifdef _WIN32
		ovi.graphics_module = "libobs-d3d11";
#else
		ovi.graphics_module = "libobs-opengl";
#endif
	}

	if (p.use_base_res) {
		ovi.base_width = p.base_cx;
		ovi.base_height = p.base_cy;
	}
	if (p.use_output_res) {
		ovi.output_width = p.output_cx;
		ovi.output_height = p.output_cy;
	}
	if (p.use_fps) {
		ovi.fps_num = p.fps;
		ovi.fps_den = 1;
	}

	const int r = obs_reset_video(&ovi);
	switch (r) {
	case OBS_VIDEO_SUCCESS:
		persist_video_config(p, ovi);
		ARO_LOG(LOG_INFO,
			"Applied video: base=%ux%u output=%ux%u fps=%u/%u",
			ovi.base_width, ovi.base_height, ovi.output_width,
			ovi.output_height, ovi.fps_num, ovi.fps_den);
		return VideoApplyResult::Applied;
	case OBS_VIDEO_CURRENTLY_ACTIVE:
		return VideoApplyResult::BlockedActive;
	default:
		ARO_LOG(LOG_WARNING, "obs_reset_video failed (code %d)", r);
		return VideoApplyResult::Failed;
	}
}

// ---------------------------------------------------------------------------
// Top-level apply
// ---------------------------------------------------------------------------

void aro_apply_preset_for_scene(obs_source_t *scene)
{
	if (!scene)
		return;

	const ScenePreset p = preset_load(scene);
	if (!p.enabled)
		return;

	const char *scene_name = obs_source_get_name(scene);

	// Recording config can always be staged for the next recording.
	apply_recording_config(p);
	apply_recording_bitrate(p);

	const VideoApplyResult vr = apply_video(p);

	switch (vr) {
	case VideoApplyResult::Applied:
		report(std::string("Applied output settings for scene '") +
		       (scene_name ? scene_name : "") + "'.");
		break;

	case VideoApplyResult::BlockedActive:
		if (p.restart_recording && obs_frontend_recording_active() &&
		    !obs_frontend_streaming_active() &&
		    !obs_frontend_virtualcam_active()) {
			// Only a recording is holding the video pipeline; we can
			// stop it, change video, and start a fresh recording.
			ARO_LOG(LOG_INFO,
				"Scene '%s': restarting recording to apply video changes",
				scene_name ? scene_name : "");
			report("Restarting recording to apply new resolution...");

			if (g_pending_restart_scene)
				obs_weak_source_release(g_pending_restart_scene);
			g_pending_restart_scene = obs_source_get_weak_source(scene);
			obs_frontend_recording_stop();
		} else {
			ARO_LOG(LOG_WARNING,
				"Scene '%s': cannot change resolution/FPS while an output is active",
				scene_name ? scene_name : "");
			report("Recording/streaming active: resolution & FPS "
			       "changes are deferred (OBS limitation). "
			       "Recording folder/format/tracks still apply to "
			       "the next recording.");
		}
		break;

	case VideoApplyResult::Failed:
		report("Failed to apply output settings (see log).");
		break;

	case VideoApplyResult::NoChange:
		break;
	}
}

void aro_on_recording_stopped()
{
	if (!g_pending_restart_scene)
		return;

	obs_source_t *scene = obs_weak_source_get_source(g_pending_restart_scene);
	obs_weak_source_release(g_pending_restart_scene);
	g_pending_restart_scene = nullptr;

	if (!scene)
		return;

	const ScenePreset p = preset_load(scene);
	apply_video(p);             // pipeline is now idle
	apply_recording_config(p);  // re-assert in case anything reset it
	apply_recording_bitrate(p);
	obs_source_release(scene);

	// Begin a new recording with the applied settings.
	obs_frontend_recording_start();
	report("Recording restarted with the new output settings.");
}

void aro_shutdown()
{
	if (g_pending_restart_scene) {
		obs_weak_source_release(g_pending_restart_scene);
		g_pending_restart_scene = nullptr;
	}
	g_status_reporter = nullptr;
}
