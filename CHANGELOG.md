# Changelog

## 1.1.0 - 2026-06-27

### Added

- Added compact dock mode, which collapses the OBS dock to a single `Mute` / `Hear` button.
- Added a right-click compact-dock menu item to restore the full settings panel.
- Added public English documentation and GitHub Pages user guide.

### Fixed

- Fixed Windows `Mute to me` so it targets OBS playback sessions only on the current monitoring endpoint.
- Restored previously muted Windows audio sessions to their original mute state when `Mute to me` is disabled.
- Avoided muting OBS monitoring output globally when the user only wants to stop hearing monitored audio locally.

### Changed

- Replaced the experimental floating mute overlay with OBS dock-native compact mode.
- Clarified installation, usage, monitoring behavior, and build instructions.

## 1.0.0 - 2026-06-23

- Initial public version.
