# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Added — Housekeeping & policy (dev, #31/#33)
- Issue-post attribution now carries the git commit identity as a trailer-style footer
  line (`Co-authored-by: Hermes Agent <hermes-agent@240mp.local> — automated agent post`);
  the marker phrase keeps the issue-feed watcher's detection contract unchanged (#31).
- Artwork & attribution policy defined in `.hermes.md` (AI art = testing only; production
  art human-made + libre, attribution mandatory) and `THANKS.md` created with verified
  provenance for the bundled fonts, the RetroArch-derived module mark, and base
  dependencies (#33). Weather condition-icon provenance flagged pending.

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

