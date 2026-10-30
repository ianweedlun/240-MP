# Plan: resolution-aware direct-play / transcode (Plex)

**Branch:** `feature/plex-res-direct-play` (forks from `dev`)
**Status:** planning only — no code changes staged yet
**Tracks:** GitHub issue on fork (resolution-based playback decision)

## Current behavior
- `modules/plex/manifest.json` exposes `video_quality` as `list_single`, options from
  `getVideoQualities` (PMS dynamic bitrate ladder) — picking anything other than
  the `"auto"` default forces transcoding **always**, regardless of source.
- Decision point: `src/modules/plex/PlexBackend.cpp` `getItemDetails` path (~line 1919):
  `bool forceTranscode = (videoQuality() != "auto");`
- Fallback chain exists already: direct play fails at playback time →
  `Player.qml` retries via `request_transcode` (keep this).

## Desired behavior
`video_quality` becomes a **resolution threshold**:
- Value `"auto"` → unchanged (always direct play; keep as the option currently labeled auto — relabel to "Direct play (always)").
- Selecting a ladder entry (e.g. `1080p - 3.5 Mb/s`) means: source **at or below** that
  resolution → direct play; **above** it (4K+) → transcode to that ladder target.
- So the existing dynamic option list stays; only its semantics change from
  "always transcode at quality X" to "direct play at/below X, transcode above X".

## Decision inputs
- PMS `/library/metadata` `Media.videoResources`/`VideoStream` carries `width`/`height`
  — backend already parses the `VideoStream` element (videoCodec, etc.); extend parse
  to capture width/height into the stream map (verify exact key casing on the wire,
  PMS XML attributes are `width`/`height`).
- Compare `height` against the threshold's resolution class. Ladder labels in
  `getVideoQualities` embed the class ("720p - 2.8 Mb/s") — parse the class out of
  the option string, or match the index against `transcodeVideoResolutions` from
  the config capability (line ~207 of vendored openapi: the PMS returns the ladder
  and resolution lists side-by-side; prefer the paired list over string parsing).

## Decision rule (pseudocode)
```
threshold = thresholdOf(videoQuality())        // none if "auto"
if threshold == none:            direct play           (current "auto")
elif source.height <= threshold: direct play
else:                            transcode(target = videoQuality())
metadata missing (no width/height):  treat as above-threshold (transcode) —
                                    safer for CRT output; log a qWarning once.
```
Rotation edge case: some MP4s store rotated display metadata; use
`max(width,height)` vs `min` consistently — pick `height` but fall back sensibly,
document when implemented.

## Unchanged on purpose
- Live TV (`LivePlayer.qml`) and `Extras.qml` "seek from start" paths stay always-transcode.
- Direct-play runtime failure → transcode retry fallback.
- Audio/subtitle stream selection logic.

## Touch points
1. `modules/plex/manifest.json` — option labels only (relabel "auto" → "Direct play (always)"); options_source stays dynamic.
2. `PlexBackend.cpp` — stream parse: width/height; replace `forceTranscode` line with the rule above; keep the qDebug decision line (extend it).
3. `Player.qml` / `Item.qml` / `QueuePlay.qml` / `CardPlay.qml` — they call `request_transcode` vs direct play based on what the backend signals; verify the detail payload already carries the decision (it returns `playback` info to QML — extend with `decision: "direct"|"transcode"` so views stop second-guessing).

## Tests
- No plex test target exists; add a tiny testable pure function
  `playbackDecision(quality, sourceW, sourceH)` in PlexBackend and a
  `plex-playback-decision` ctest target (mirrors `weather-codes` pattern), so CI
  gates this before it reaches the Pi.

## Validation on Pi (project rules)
- Deploy arm64 build, reboot, F12 screenshot sweep: play a 1080p item (direct,
  expect no transcode log line), a 4K item (expect transcode to the ladder target),
  and "Direct play (always)" with the 4K item (expect direct / failure-fallback).
  Decision qDebug lines go through journalctl on the Pi.

## As built (756e59e → this branch)
- Implemented as a header-only pure function (`src/modules/plex/PlaybackDecision.h`,
  `PlexQuality::shouldTranscode`) — QML untouched: `buildItemDetail` still emits
  the `forceTranscode` key, only its computation changed.
- Ladder labels carry NO bitrate ("Above 1080p: transcode") — the old id→Mbps
  pairings were unverified and user rejected them; ids still feed
  `maxVideoBitrate` internally, target quality is an internal detail for now.
- Test target `plex-playback-decision` (ctest-registered, header-only so it links
  nothing); needs `Test` added to root `find_package(Qt6 …)` (same delta the
  retroarch branch made; a dev↔branch CMake merge will see this region conflict).
- Unknown id not on the ladder = legacy always-transcode (back-compat for old
  config.json values). Unknown dimensions = transcode (conservative).
- Decision test verified locally: 8/8 green (x86_64, Qt 6.10.2).

## Not done / open
- Pi validation sweep (needs deploy + reboot + screenshots) — when this lands.
- Actual transcode target quality per ladder tier remains the hardcoded ids
  (8000/4000/2000 kbps). If PMS caps resolution by bitrate with the Chrome
  profile, this may be fine as-is; revisit if 4K→target output looks wrong on CRT.

