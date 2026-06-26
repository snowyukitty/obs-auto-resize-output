// SPDX-License-Identifier: GPL-2.0-or-later

#include "ApplyPreset.hpp"
#include "plugin-log.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#endif

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#ifdef _WIN32
#include <vector>
#endif

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------

// Set when a video override is deferred until the current recording stops.
static obs_weak_source_t *g_pending_restart_scene = nullptr;

// "Mute to me" state. On Windows, this uses the same layer EarTrumpet uses:
// the OBS process' Windows audio playback sessions. That silences monitoring
// playback without touching OBS' internal recording/streaming audio path.
//
// On non-Windows platforms we retain the older fallback of pointing OBS's
// GLOBAL monitoring device at a sentinel. We deliberately do NOT toggle
// per-source monitoring TYPE: obs_save_source() persists monitoring_type into
// the scene collection on every save (auto-save, collection switch, and the
// exit save, which runs before any restore hook), so a save while muted would
// permanently wipe the user's "Monitor and Output" setup.
enum class MuteBackend {
	None,
	WindowsAudioSession,
	SilentMonitorDevice,
};

static bool g_muted_to_me = false;
static MuteBackend g_mute_backend = MuteBackend::None;
// The device to return to on un-mute (captured when muting; updated if a scene
// requests a different monitoring device while muted).
static std::string g_saved_monitor_name;
static std::string g_saved_monitor_id;

// Sentinel monitoring device: a non-empty, non-"default" id that matches no real
// device. OBS accepts this value, but some monitor backends keep the old live
// monitor if reset fails, so this is only a non-Windows fallback.
static constexpr const char *kSilentMonitorName = "Auto Resize Output (muted to you)";
static constexpr const char *kSilentMonitorId = "aro::muted-to-you::silent";

static StatusReporter g_status_reporter;

#ifdef _WIN32
struct SavedAudioSessionMute {
	std::wstring instance_id;
	bool muted = false;
};

static std::vector<SavedAudioSessionMute> g_saved_audio_session_mutes;
#endif

void aro_set_status_reporter(StatusReporter reporter)
{
	g_status_reporter = std::move(reporter);
}

static void report(const std::string &msg)
{
	if (g_status_reporter)
		g_status_reporter(msg);
}

#ifdef _WIN32
template<typename T> static void release_com(T *&ptr)
{
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

class ScopedComInit {
public:
	ScopedComInit()
	{
		m_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		m_uninit = SUCCEEDED(m_hr);
	}

	~ScopedComInit()
	{
		if (m_uninit)
			CoUninitialize();
	}

	bool ok() const
	{
		return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
	}

	HRESULT result() const { return m_hr; }

private:
	HRESULT m_hr = S_OK;
	bool m_uninit = false;
};

static SavedAudioSessionMute *find_saved_audio_session_mute(const std::wstring &instance_id)
{
	auto it = std::find_if(g_saved_audio_session_mutes.begin(), g_saved_audio_session_mutes.end(),
			       [&](const SavedAudioSessionMute &saved) {
				       return saved.instance_id == instance_id;
			       });
	return it == g_saved_audio_session_mutes.end() ? nullptr : &*it;
}

template<typename Func> static bool for_each_obs_audio_session(Func &&func, int &matched_sessions)
{
	matched_sessions = 0;

	ScopedComInit com;
	if (!com.ok()) {
		ARO_LOG(LOG_WARNING, "CoInitializeEx failed while muting OBS audio sessions: 0x%08lX",
			(unsigned long)com.result());
		return false;
	}

	IMMDeviceEnumerator *device_enum = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&device_enum));
	if (FAILED(hr)) {
		ARO_LOG(LOG_WARNING, "Failed to create IMMDeviceEnumerator: 0x%08lX", (unsigned long)hr);
		return false;
	}

	IMMDeviceCollection *devices = nullptr;
	hr = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
	if (FAILED(hr)) {
		ARO_LOG(LOG_WARNING, "Failed to enumerate render endpoints: 0x%08lX", (unsigned long)hr);
		release_com(device_enum);
		return false;
	}

	UINT device_count = 0;
	hr = devices->GetCount(&device_count);
	if (FAILED(hr)) {
		ARO_LOG(LOG_WARNING, "Failed to count render endpoints: 0x%08lX", (unsigned long)hr);
		release_com(devices);
		release_com(device_enum);
		return false;
	}

	const DWORD current_pid = GetCurrentProcessId();
	bool ok = true;

	for (UINT i = 0; i < device_count; ++i) {
		IMMDevice *device = nullptr;
		hr = devices->Item(i, &device);
		if (FAILED(hr))
			continue;

		IAudioSessionManager2 *manager = nullptr;
		hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&manager);
		release_com(device);
		if (FAILED(hr))
			continue;

		IAudioSessionEnumerator *sessions = nullptr;
		hr = manager->GetSessionEnumerator(&sessions);
		release_com(manager);
		if (FAILED(hr))
			continue;

		int session_count = 0;
		hr = sessions->GetCount(&session_count);
		if (FAILED(hr)) {
			release_com(sessions);
			continue;
		}

		for (int j = 0; j < session_count; ++j) {
			IAudioSessionControl *control = nullptr;
			hr = sessions->GetSession(j, &control);
			if (FAILED(hr))
				continue;

			IAudioSessionControl2 *control2 = nullptr;
			hr = control->QueryInterface(IID_PPV_ARGS(&control2));
			release_com(control);
			if (FAILED(hr))
				continue;

			DWORD pid = 0;
			hr = control2->GetProcessId(&pid);
			if (FAILED(hr) || pid != current_pid) {
				release_com(control2);
				continue;
			}

			ISimpleAudioVolume *volume = nullptr;
			hr = control2->QueryInterface(IID_PPV_ARGS(&volume));
			if (FAILED(hr)) {
				release_com(control2);
				continue;
			}

			LPWSTR raw_instance_id = nullptr;
			std::wstring instance_id;
			hr = control2->GetSessionInstanceIdentifier(&raw_instance_id);
			if (SUCCEEDED(hr) && raw_instance_id)
				instance_id = raw_instance_id;
			if (raw_instance_id)
				CoTaskMemFree(raw_instance_id);
			if (instance_id.empty())
				instance_id = L"obs-session-" + std::to_wstring(matched_sessions);

			++matched_sessions;
			if (!func(volume, instance_id))
				ok = false;

			release_com(volume);
			release_com(control2);
		}

		release_com(sessions);
	}

	release_com(devices);
	release_com(device_enum);
	return ok;
}

static bool set_windows_obs_audio_session_mute(bool mute, bool capture_previous)
{
	if (mute && capture_previous)
		g_saved_audio_session_mutes.clear();

	int matched_sessions = 0;
	const bool ok = for_each_obs_audio_session(
		[&](ISimpleAudioVolume *volume, const std::wstring &instance_id) {
			if (mute) {
				if (!find_saved_audio_session_mute(instance_id)) {
					BOOL was_muted = FALSE;
					HRESULT hr = volume->GetMute(&was_muted);
					if (FAILED(hr)) {
						ARO_LOG(LOG_WARNING, "Failed to read OBS audio session mute state: 0x%08lX",
							(unsigned long)hr);
						return false;
					}
					g_saved_audio_session_mutes.push_back({instance_id, was_muted != FALSE});
				}

				HRESULT hr = volume->SetMute(TRUE, nullptr);
				if (FAILED(hr)) {
					ARO_LOG(LOG_WARNING, "Failed to mute OBS audio session: 0x%08lX",
						(unsigned long)hr);
					return false;
				}
				return true;
			}

			const SavedAudioSessionMute *saved = find_saved_audio_session_mute(instance_id);
			const BOOL restore_mute = saved && saved->muted ? TRUE : FALSE;
			HRESULT hr = volume->SetMute(restore_mute, nullptr);
			if (FAILED(hr)) {
				ARO_LOG(LOG_WARNING, "Failed to restore OBS audio session mute state: 0x%08lX",
					(unsigned long)hr);
				return false;
			}
			return true;
		},
		matched_sessions);

	if (!ok)
		return false;

	if (matched_sessions == 0) {
		ARO_LOG(LOG_WARNING, "No OBS Windows audio playback session found for mute-to-me");
		return false;
	}

	return true;
}
#endif

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
		config_set_string(cfg, section, "RecFormat2", p.rec_format.c_str());
		changed = true;
	}

	if (p.use_audio_tracks) {
		// Bitmask of enabled recording tracks (bit0 = track 1).
		config_set_int(cfg, section, "RecTracks", p.audio_tracks);
		changed = true;
	}

	if (changed) {
		config_save(cfg);
		ARO_LOG(LOG_INFO, "Applied recording config (mode=%s) for upcoming recordings",
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
	const char *mode = cfg ? config_get_string(cfg, "Output", "Mode") : nullptr;
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	if (!advanced) {
		ARO_LOG(LOG_WARNING, "Recording bitrate override needs Advanced output mode; ignored in Simple mode");
		return;
	}

	char *profile_path = obs_frontend_get_current_profile_path();
	if (!profile_path)
		return;
	const std::string path = std::string(profile_path) + "/recordEncoder.json";
	bfree(profile_path);

	// Preserve any existing encoder settings; only override rate control.
	obs_data_t *enc = obs_data_create_from_json_file(path.c_str());
	if (!enc)
		enc = obs_data_create();
	obs_data_set_string(enc, "rate_control", "CBR");
	obs_data_set_int(enc, "bitrate", p.rec_bitrate);
	obs_data_save_json_safe(enc, path.c_str(), "tmp", "bak");
	obs_data_release(enc);

	ARO_LOG(LOG_INFO, "Set recording encoder to CBR %u kbps for upcoming recordings", p.rec_bitrate);
}

// ---------------------------------------------------------------------------
// Audio monitoring device (the device YOU hear)
// ---------------------------------------------------------------------------

// Switch OBS's global audio monitoring device. Monitoring is purely a playback
// path: it feeds audio to a device for you to listen to and is completely
// separate from the encoder/output path that recording and streaming use.
// Re-pointing it therefore takes effect immediately, never interrupts an active
// recording, and never changes what is recorded or at what volume. This lets a
// scene send monitored audio to a device you are not listening to (so you stop
// hearing it) while the recording keeps capturing it untouched.
static void apply_monitoring_device(const ScenePreset &p)
{
	if (!p.use_monitor_device)
		return;

	if (!obs_audio_monitoring_available()) {
		ARO_LOG(LOG_WARNING, "Audio monitoring is not available on this system; monitor device ignored");
		return;
	}

	// While using the fallback silent monitoring device, the live device must
	// stay on the sentinel; remember the scene's choice as the device to
	// restore on un-mute. Windows audio-session mute can safely let the device
	// change, then re-apply the process-session mute.
	if (g_muted_to_me && g_mute_backend == MuteBackend::SilentMonitorDevice) {
		g_saved_monitor_name = p.monitor_device_name;
		g_saved_monitor_id = p.monitor_device_id;
		ARO_LOG(LOG_INFO, "Muted to you: deferring scene monitor device '%s' until un-mute",
			p.monitor_device_name.empty() ? p.monitor_device_id.c_str() : p.monitor_device_name.c_str());
		return;
	}

	if (obs_set_audio_monitoring_device(p.monitor_device_name.c_str(), p.monitor_device_id.c_str())) {
		ARO_LOG(LOG_INFO, "Set audio monitoring device to '%s'",
			p.monitor_device_name.empty() ? p.monitor_device_id.c_str() : p.monitor_device_name.c_str());
#ifdef _WIN32
		if (g_muted_to_me && g_mute_backend == MuteBackend::WindowsAudioSession)
			set_windows_obs_audio_session_mute(true, false);
#endif
	} else {
		ARO_LOG(LOG_WARNING, "Failed to set audio monitoring device to '%s' (id '%s')",
			p.monitor_device_name.c_str(), p.monitor_device_id.c_str());
	}
}

// ---------------------------------------------------------------------------
// Video (obs_reset_video)
// ---------------------------------------------------------------------------

// Mirror applied video values into the profile config so the Settings dialog
// and the next launch reflect what is actually running.
static void persist_video_config(const ScenePreset &p, const struct obs_video_info &ovi)
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
		ARO_LOG(LOG_INFO, "Applied video: base=%ux%u output=%ux%u fps=%u/%u", ovi.base_width, ovi.base_height,
			ovi.output_width, ovi.output_height, ovi.fps_num, ovi.fps_den);
		return VideoApplyResult::Applied;
	case OBS_VIDEO_CURRENTLY_ACTIVE:
		return VideoApplyResult::BlockedActive;
	default:
		ARO_LOG(LOG_WARNING, "obs_reset_video failed (code %d)", r);
		return VideoApplyResult::Failed;
	}
}

// ---------------------------------------------------------------------------
// "Mute to me" (global monitoring toggle)
// ---------------------------------------------------------------------------

// Point monitoring back at the saved device (or "Default" if none was saved).
static void restore_monitor_device()
{
	const char *name = g_saved_monitor_name.empty() ? "Default" : g_saved_monitor_name.c_str();
	const char *id = g_saved_monitor_id.empty() ? "default" : g_saved_monitor_id.c_str();
	obs_set_audio_monitoring_device(name, id);
}

bool aro_mute_to_me_active()
{
	return g_muted_to_me;
}

void aro_set_mute_to_me(bool mute)
{
	if (!obs_audio_monitoring_available()) {
		ARO_LOG(LOG_WARNING, "Audio monitoring not available; 'mute to me' has no effect");
		return;
	}
	if (mute == g_muted_to_me)
		return;

	if (mute) {
#ifdef _WIN32
		if (set_windows_obs_audio_session_mute(true, true)) {
			g_mute_backend = MuteBackend::WindowsAudioSession;
			g_muted_to_me = true;
			ARO_LOG(LOG_INFO, "Mute-to-me ON: muted OBS Windows audio playback sessions; recording unaffected");
			report("Muted to you: OBS playback is muted in Windows. Recording continues at full volume.");
			return;
		}

		if (!g_saved_audio_session_mutes.empty()) {
			set_windows_obs_audio_session_mute(false, false);
			g_saved_audio_session_mutes.clear();
		}

		ARO_LOG(LOG_WARNING, "Mute-to-me could not find/mute an OBS Windows audio playback session");
		report("Could not mute to you: Windows has no active OBS playback session to mute. Start monitored audio, then try again.");
		return;
#else
		// Remember the device we're currently listening on, then send
		// monitoring to the silent sentinel. The output/encoder path is
		// untouched, so recording continues unchanged.
		const char *curName = nullptr;
		const char *curId = nullptr;
		obs_get_audio_monitoring_device(&curName, &curId);
		g_saved_monitor_name = curName ? curName : "";
		g_saved_monitor_id = curId ? curId : "";
		obs_set_audio_monitoring_device(kSilentMonitorName, kSilentMonitorId);
		g_mute_backend = MuteBackend::SilentMonitorDevice;
		g_muted_to_me = true;
		ARO_LOG(LOG_INFO, "Mute-to-me ON: monitoring silenced; recording unaffected");
		report("Muted to you: you no longer hear monitored audio. Recording continues at full volume.");
#endif
	} else {
#ifdef _WIN32
		if (g_mute_backend == MuteBackend::WindowsAudioSession) {
			set_windows_obs_audio_session_mute(false, false);
			g_saved_audio_session_mutes.clear();
		} else
#endif
		if (g_mute_backend == MuteBackend::SilentMonitorDevice) {
			restore_monitor_device();
		}
		g_mute_backend = MuteBackend::None;
		g_muted_to_me = false;
		ARO_LOG(LOG_INFO, "Mute-to-me OFF: monitoring playback restored");
		report("Unmuted to you: monitoring restored.");
	}
}

// Defensive recovery at startup: if a previous run left the sentinel device
// persisted (e.g. OBS was killed while muted), reset to Default so the user is
// never silently stuck without monitoring.
void aro_recover_monitoring_on_load()
{
	if (!obs_audio_monitoring_available())
		return;
	const char *id = nullptr;
	obs_get_audio_monitoring_device(nullptr, &id);
	if (id && strcmp(id, kSilentMonitorId) == 0) {
		ARO_LOG(LOG_INFO, "Recovered leftover 'muted to you' monitoring device -> Default");
		obs_set_audio_monitoring_device("Default", "default");
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

	// Monitoring device is independent of the output pipeline; apply it always
	// (it is safe even while recording/streaming).
	apply_monitoring_device(p);

	const VideoApplyResult vr = apply_video(p);

	switch (vr) {
	case VideoApplyResult::Applied:
		report(std::string("Applied output settings for scene '") + (scene_name ? scene_name : "") + "'.");
		break;

	case VideoApplyResult::BlockedActive:
		if (p.restart_recording && obs_frontend_recording_active() && !obs_frontend_streaming_active() &&
		    !obs_frontend_virtualcam_active()) {
			// Only a recording is holding the video pipeline; we can
			// stop it, change video, and start a fresh recording.
			ARO_LOG(LOG_INFO, "Scene '%s': restarting recording to apply video changes",
				scene_name ? scene_name : "");
			report("Restarting recording to apply new resolution...");

			if (g_pending_restart_scene)
				obs_weak_source_release(g_pending_restart_scene);
			g_pending_restart_scene = obs_source_get_weak_source(scene);
			obs_frontend_recording_stop();
		} else {
			ARO_LOG(LOG_WARNING, "Scene '%s': cannot change resolution/FPS while an output is active",
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
	apply_video(p);            // pipeline is now idle
	apply_recording_config(p); // re-assert in case anything reset it
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

	// Undo any "mute to me" so OBS playback/session state is not left muted by
	// this plugin.
	if (g_muted_to_me) {
#ifdef _WIN32
		if (g_mute_backend == MuteBackend::WindowsAudioSession) {
			set_windows_obs_audio_session_mute(false, false);
			g_saved_audio_session_mutes.clear();
		} else
#endif
		if (g_mute_backend == MuteBackend::SilentMonitorDevice) {
			restore_monitor_device();
		}
		g_mute_backend = MuteBackend::None;
		g_muted_to_me = false;
	}

	g_status_reporter = nullptr;
}
