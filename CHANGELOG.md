# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Fixed — YouTube Subscriptions freshness (feature/youtube-finish)
- Signed-in Subscriptions stopped riding YouTube's TV-client `FEsubscriptions` browse: that feed is a **server-precomputed cache** — live probe 2026-11-04 showed 28/30 sampled channels' newest uploads entirely absent from its 43 unordered tiles (oldest-of-newest tile was 13 h late) while the official mobile app showed them. Signed-in feed now = one fresh `FEchannels` roster hop + wave-capped (32 concurrent) per-channel RSS union sorted by publish date — same freshness source the official app uses, TTL-cached (15 min), signed-out file path untouched. State-log rows label now `union` when signed in.

### Fixed — YouTube playback judder on the CRT rig (2026-11-03, operational)
- Long uploads ("YOU CAN SEE EVERYTHING", 76 min) stuttered while audio and position tracked normally: `video-sync=display-resample` in the Pi's `~/.config/mpv/mpv.conf` fought the vc4/CRT refresh (blend cost + unreliable vsync timing — the same subsystem that once froze startup). Switched to `video-sync=audio` (framedrop default): telemetry showed RTF 1.02 and demux cache 150–160 s both before and after, so the judder was presentation-level, not network or decode starvation. Runtime config change; no rebuild, re-read per mpv spawn.

### Fixed — YouTube Channels naming (feature/youtube-finish)
- Channels view fell back to raw UC IDs whenever per-channel RSS name fetches failed (311 round-trips on open). Signed-in path now uses one InnerTube `FEchannels` browse: names + IDs for all subscribed channels in a single request (live probe: 313 tiles); RSS stays the legacy signed-out fallback. `ytui::extractChannelSyncRows` handles TV tileRenderer + web channelRenderer + base64-protobuf params id fallback (2 new unit tests).

### Added — YouTube SponsorBlock + latency telemetry (feature/youtube-finish)
- SponsorBlock per-category controls (`SponsorBlock: <Category>` rows, Off/Mark/Skip): sponsor and self-promo default to Skip, the rest to Off. Skips are cut out of an mpv EDL virtual timeline (both media halves identically, audio stays in sync across jumps); Mark keeps playback whole with titled chapter boundaries in the OSC. Segments fetched k-anonymously (4-hex SHA-256 prefix, exact videoID filter, Red doctrine), cached per video, pre-warmed for whole list pages. 10/10 yt-sponsor-edl tests + live mpv IPC probes prove the EDL arithmetic.
- Journal telemetry for playback + browse (`yt-play path=warm|cold`, `yt-resolve`, `yt-fallback`, `yt-sponsor`, `yt-rows path=account|file rows=N` in the state log; `[Youtube] resolve/warm/sponsor` lines with ms in the service journal) — per-pick latency tables replace anecdote rounds; unit suite registered and green (53 checks across five test binaries).

### Added — YouTube SponsorBlock OSC marks (feature/youtube-finish)
- SponsorBlock segments now paint on the mpv OSD in SponsorBlock's own default colors (green sponsor, yellow self-promo, cyan intro, blue outro/intermission/preview teal, purple filler, violet interaction — hex table ported from ajayyy/SponsorBlock `src/config.ts`, GPL-3.0 prior art): Skip lands a solid category tick at the jump point, Mark paints a dim band over the segment. Ranges travel to the OSC as a per-launch `sb-ranges` script-opts hint (raw source seconds; the script subtracts skip-cut lengths to sit on the cut virtual timeline).
- SponsorBlock settings moved from eight module-settings rows to one "SponsorBlock" submenu row with short per-category labels; sponsor + self-promo skip silently by default, everything else off.
- `[Youtube]` service-journal lines switched to qWarning so the release build actually shows them (qDebug is filtered); explains the missing resolve/sponsor evidence in earlier runs.

### Added — YouTube account playlists (feature/youtube-account-playlists)
- "Playlists" row between Channels and Watch Later is now account-driven when signed in via the TV device flow: the account's Playlists tab (YouTube's own tab order, recent activity first) with drill-down into a playlist's videos through the account browse; file playlists (youtube_playlists.txt) unchanged when signed out, and every account failure falls back to the file path. 18/18 yt-browse-parse tests green.

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

