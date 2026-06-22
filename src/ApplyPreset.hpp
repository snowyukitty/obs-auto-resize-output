// SPDX-License-Identifier: GPL-2.0-or-later
//
// ApplyPreset: turns a ScenePreset into actual OBS state.
//
//  * Video (base/output resolution, FPS) -> obs_reset_video(). This only
//    succeeds while no output is active; libobs returns OBS_VIDEO_CURRENTLY_ACTIVE
//    otherwise. We mirror the applied values into the profile config so the
//    Settings UI and on-disk state stay consistent.
//
//  * Recording (folder, container format, audio tracks) -> the active profile
//    config_t. OBS reads these when you press "Start Recording", so we write
//    them on scene change and they apply to the next recording.

#pragma once

#include "ScenePreset.hpp"

#include <functional>
#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;

enum class VideoApplyResult {
	NoChange,      // preset requested no video override
	Applied,       // obs_reset_video succeeded
	BlockedActive, // an output is active; cannot change video now
	Failed,        // obs_reset_video returned an unexpected error
};

// Optional UI status sink. The dock registers this to surface the result of
// the most recent automatic apply to the user.
using StatusReporter = std::function<void(const std::string &message)>;
void aro_set_status_reporter(StatusReporter reporter);

// Apply a single scene's preset (recording config always; video if idle).
// Handles the optional "restart recording to apply" path.
void aro_apply_preset_for_scene(obs_source_t *scene);

// Called from the frontend RECORDING_STOPPED handler to complete a pending
// stop -> apply -> start restart sequence.
void aro_on_recording_stopped();

// Release any pending state (call on module unload / exit).
void aro_shutdown();
