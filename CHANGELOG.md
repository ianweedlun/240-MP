# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Changed — Settings: consolidated playback-behavior settings into a Playback section (#18, feature/mpv-conf-direct-edit)
- One section header ("Playback") groups every mpv-owned playback control — 1080p Playback tradeoff, Auto Crop, Video Levels, the four mpv.conf knobs (Audio Normalize, Sound Boost, Deinterlace, Audio Channels) — flat-nav preserved (section header, not nested submenu). Screen Saver stays outside (burn-in idle aid, not playback behavior); Color Scheme / Start on Module are shell behavior.

### Added — mpv.conf as single source of truth for basic mpv knobs (#26, feature/mpv-conf-direct-edit)
- The four basic mpv knobs read and write `~/.config/mpv/mpv.conf` directly (last-assignment-wins parse, comments skipped, underscore spellings tolerated, one-level `include` followed, byte-preserving in-place write-back); the "Mpv Conf" pseudo-value and config.json override layer are gone, with a one-time migration of legacy app-section keys at startup; out-of-menu values display honestly as `Custom (<raw>)`; mpv spawn args unchanged. Pure parser covered by unit tests.

### Fixed — Weather module OSD behaviors (feature/weather-osd-signals, #17)
- Opening the module no longer shows the volume-bar OSD (bar now appears only on real volume-key changes; the old `musicStateChanged` signal fired from autostarted music at open).
- Volume clamps at 0/MUTE instead of bouncing to 50 (`musicVolume || 50` treated 0 as falsy — the reported wrap-to-middle).
- Backed by a volume-only `musicVolumeChanged(int)` signal split; 10/10 weather unit tests green.

### Added — RetroArch full functionality (feature/retroarch-full-functionality)
- Live playlist sort (SYSTEM/ALPHA/COMPANY) re-applied on settings change; short-name section headers; playlist game counts.
- thumbnail_display OFF/LEFT/RIGHT zone geometry for boxart placement (game list only); playlist views stay plain.
- Module-exit navigation: exiting the module (or after a game) returns to the main menu, not Settings; game-key suppression while a RetroArch child owns the display.
- Lazy thumbnail resolution (snappy opening of ~4,700-item playlists); archive rom paths (`zip#entry`) resolve + launch (verified on CRT with '10-Yard Fight').
- Debug telemetry behind a TELEMETRY compile gate: /tmp/240mp_state.log (loads/navigate/back/row-select/key-suppression lines), screenshot view-tagging, debug key channel; RowTrackingListView core primitive. 23/23 unit tests green.

### Added — Tooling & pipeline
- Offscreen validation fast path (QT_QPA_PLATFORM=offscreen + synthetic key channel; pi-nav.sh `local`, pi-keys.py).
- CI direct-deploy chain to the Pi (ghdeploy key-only + reboot dance), paths-ignore so markdown-only pushes queue no builds; separate ci-macos.yml; convergence watcher.
- Comms pipeline: fork-issue channel with the maintainer (kickoff #19) + distinct Hermes commit identity (env-based; documented in .hermes.md and skill references).

### Changed
- .hermes.md single-sourced on dev, blob-mirrored to every branch (sync-hermes.yml + proven plumbing fallback).
- Documentation consolidation: one home per fact — `.hermes.md` is contract-only (policy + resource map + tiering rule); operational mechanics live in the agent skill's references; `docs/plans/*` retired (plans/specs live in fork issues only); `CLAUDE.md` is a hub pointing at the contract; dated reports frozen. `docs-mirror.sh` (plumbing-fixed: base on `refs/remotes/origin/*`) re-synced every branch.

