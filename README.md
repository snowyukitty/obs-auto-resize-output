# Auto Resize Output

OBS Studio plugin for **per-scene output, recording, and monitoring presets**.

Auto Resize Output lets each scene carry its own output/recording preferences. When you switch scenes, the plugin applies that scene's enabled overrides and stores the preset directly in the scene source's `private_settings`, so duplicated scenes keep their preset automatically.

## Features

| Setting | When it applies | Notes |
|---|---|---|
| Base/canvas resolution | On scene switch, only while OBS outputs are idle | Uses `obs_reset_video()` |
| Output/scaled resolution | On scene switch, only while OBS outputs are idle | Same pipeline limitation |
| Integer FPS | On scene switch, only while OBS outputs are idle | Same pipeline limitation |
| Recording folder | Next recording | Writes the active OBS profile config |
| Recording format | Next recording | `mkv`, `hybrid_mp4`, `mp4`, `mov`, `fragmented_mp4`, `fragmented_mov`, `mpegts`, `flv` |
| Recording audio tracks | Next recording | Six-track bitmask |
| Recording video bitrate | Next recording | Advanced output mode only; writes `recordEncoder.json` as CBR |
| Audio monitoring device | Immediately, even while recording | Changes the OBS monitoring playback device only |

Global controls:

- **Mute to me**: one click stops *you* from hearing OBS-monitored audio while recording/streaming keeps capturing it at full volume.
- **Compact dock mode**: collapses the OBS dock to a single `Mute` / `Hear` button. Right-click the button and choose `Show full settings` to return to the full panel.

## Important OBS Limitation

OBS cannot change base resolution, output resolution, or FPS while recording, streaming, or virtual camera output is active. That is a libobs video pipeline limitation, not a plugin limitation.

Behavior:

- If OBS is idle, scene video overrides apply immediately.
- If an output is active, recording folder/format/tracks/bitrate are staged for the next recording, but video changes are blocked.
- If `restart recording to apply video changes` is enabled and only recording is active, the plugin can stop recording, apply the video change, and start a new recording. This creates a new file.

## Audio Monitoring And "Mute To Me"

OBS audio monitoring is the playback path that lets *you* hear sources. It is separate from the recording/streaming encoder path.

`Mute to me` only affects monitored audio:

- On Windows, it mutes OBS playback sessions on the current monitoring endpoint, similar to the audio-session layer used by EarTrumpet. It does not change source volume, track selection, monitoring type, or the recorded audio path.
- On other platforms, it falls back to routing OBS's global monitoring device to a silent sentinel device.

For this to work, the sound must be heard through OBS monitoring. In OBS, open **Advanced Audio Properties** and set the source's **Audio Monitoring** to `Monitor and Output` or `Monitor Only`.

If a sound is being played directly by the operating system to your headphones or speakers, OBS monitoring controls cannot silence it without also affecting what the system loopback capture receives.

## Usage

1. Open OBS and enable **Docks -> Auto Resize Output**.
2. Choose the scene to edit. The dock follows the current program scene until you manually select another scene.
3. Enable **per-scene overrides** for that scene.
4. Check only the settings that scene should override.
5. Optionally click **Copy from current OBS settings** to use the current profile values as a starting point.
6. Click **Apply now** to apply immediately when the edited scene is the current program scene, or just switch scenes and let the plugin apply automatically.
7. Use **Mute to me** whenever you want to stop hearing monitored audio without affecting recording.
8. Use **Compact dock mode** when you only want the mute control visible inside the OBS dock layout. Right-click the compact button and choose **Show full settings** to expand it again.

## Installation

Windows plugin layout:

```text
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\bin\64bit\obs-auto-resize-output.dll
%ProgramData%\obs-studio\plugins\obs-auto-resize-output\data\locale\en-US.ini
```

Close OBS before replacing the DLL.

## Build

This repository uses the OBS plugin-template build system. The pinned dependencies are in `buildspec.json` and currently target OBS Studio 31.1.1 with the matching OBS dependencies and Qt6 package.

Cloud build:

```powershell
gh workflow run dispatch.yaml --ref main -f job=build
```

Local Windows build with the template presets:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

See [BUILD.md](BUILD.md) for the full build guide.

## Design Notes

- Presets are stored under the scene source's `private_settings` key `auto_resize_output`.
- Only public OBS APIs are used: `obs.h`, `obs-frontend-api.h`, profile `config_t`, and `obs_data`.
- Scene duplication carries presets because libobs copies source private settings during duplication.
- The plugin deliberately does not toggle per-source monitoring type for mute. OBS persists `monitoring_type` during saves, so muting by changing source monitoring modes could permanently overwrite a user's mixer setup.

## Current Scope

Supported:

- OBS Studio 30/31-era recording config keys, with CI pinned to OBS 31.1.1.
- Windows, macOS, and Ubuntu builds through GitHub Actions.
- Advanced output mode for recording bitrate overrides.

Known limits:

- Simple output mode does not expose a clean independent recording bitrate override.
- Video changes cannot be applied while OBS outputs are active unless the plugin restarts recording.
- `Mute to me` only affects audio heard through OBS monitoring.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
