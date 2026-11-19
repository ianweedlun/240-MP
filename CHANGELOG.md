# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Fixed — players: an early Back before mpv's IPC connects no longer strands playback (R-2)
- Pressing **Back** immediately after starting a video — before mpv's IPC socket connects — used to be silently dropped: the QML players' Back handlers only call `mpvController.sendKey()`, and `sendCommand` drops every command while the socket is not yet `ConnectedState`. The just-spawned mpv kept playing with no Player view to receive its end-of-playback signal, so the app sat on the previous menu while audio/video ran to natural EOF — **forever** when Loop was on (`--loop-playlist=inf`).
- `MpvController::sendKey` now treats the quit-intent keys **ESC / BS / STOP** as guaranteed-delivery: pre-connect they call the existing `forceStop()` (terminate + guarded SIGKILL escalation), which reaps the child and emits `playbackEnded("stopped")` so the view goes back; post-connect they are forwarded as a byte-identical `keypress`, preserving the lua semantics (Back closes the OSC menu / wakes the screen saver first, and only a later press quits). All other keys and every other `sendCommand` caller are unchanged. This covers all 18 QML ESC/BS sites and the macOS inactive-window path by construction — no per-view edits.
- **YouTube** gains a `launchDone` launch-intent flag (set the instant `doPlay` spawns mpv, reset in `preparePlayback`/`playAt`). Its ESC/BS Back guard now keys off `launchDone` instead of `playbackStarted`, so a Back after launch but before the first position event routes through `sendKey` rather than `goBack()` orphaning mpv. Pre-launch windows (cold resolve, the 50 ms start timer, the resume overlay) keep their direct `goBack()`.
- Covered by new product tests (`mpv-quit-intent`: synchronous pre-connect invariant ×100 each for ESC/BS/STOP, post-connect OSC-menu/screen-saver preservation, loop-on bounded exit; `youtube-quit-intent-smoke`: the launch-guard routing) and the deterministic early-Back regression gate (RG-1..RG-4 flip FAIL→PASS). Owner CRT validation on the Pi remote is still pending.

### Fixed — player: per-instance mpv IPC socket and log, with sun_path bound guard (T2)
- `MpvController` used four process-global fixed temp paths (`$TMPDIR/240mp-mpv.sock`, `.log`, `240mp-input.conf`, `240mp-mpv-subinfo.json`). Two concurrent 240-MP instances — or two lab harnesses — collided: mpv unlinks+rebinds an existing socket, so instance B's mpv stole instance A's IPC, and the shared mpv log contaminated per-instance debug reads (torture ledger T4).
- All four paths are now per-instance, keyed by `<pid>-<seq>` under `QDir::tempPath()` (the sequence separates multiple controllers inside one process, e.g. test rigs). A new pure helper, `util/MpvInstancePaths.h`, computes the paths and enforces the `sockaddr_un.sun_path` bound by platform: 107 usable bytes on Linux, 103 on macOS.
- **Fail safe, never relocate:** if the socket path would exceed `sun_path`, IPC is disabled for that instance (`socketPath()` returns empty, `--input-ipc-server` is omitted, no connect attempts). One warning names the rejected path, its byte length, and the limit; playback continues and quit falls back to `forceStop`. A relocation to `/tmp` would silently break the discovery contract and the RG-3 padded-TMPDIR regression arm, so it is deliberately not done.
- **Discovery contract:** construction prints one unconditional log line — `[MpvController] instance paths: socket=<path|disabled> log=<path> ipc=<enabled|disabled>` — and exposes `socketPath()` / `logFilePath()` getters. Tooling and the regression harness parse the line; no symlink is left at the old fixed shared path because last-writer-wins would reintroduce collision.
- **T4 lab isolation:** `InputManager`'s debug key channel and `AppCore::note_state`'s state log now respect `240MP_KEY_FILE` and `240MP_STATE_LOG` env overrides (defaults unchanged at `/tmp/240mp_key.in` and `/tmp/240mp_state.log`). Lab harnesses can give concurrent instances distinct telemetry channels.
- `MpvQuitIntentTest` now reads the controller's actual per-instance socket path instead of the old hardcoded shared name. WeatherBackend's comment is updated.
- Covered by new unit tests: `mpv-instance-paths` (pure helper, including the known-bad padded-TMPDIR case and UTF-8 byte counting) and `mpv-instance-socket` (real `MpvController` construction, offscreen, no mpv spawn, including two controllers in one process and the over-limit disabled path). Branched from `feature/r2-early-back` @ adcdeac (#109) because `dev` has no quit-intent fallback; merge dependency on #109. Owner CRT validation pending.

### Added — audio: non-blocking mixer process seam (T1 P-5A infrastructure)
- New `audio::MixerProcessRunner` (`src/audio/MixerProcessRunner.*`): the shared asynchronous subprocess + state-cache seam the ALSA and PipeWire volume backends will use. All `QProcess`/timer activity lives on a dedicated worker thread; `submit()` and the cached `level`/`mute` accessors are thread-safe, never spawn on the caller thread, and return in well under a frame budget (unit-tested < 16 ms with a 200 ms child in flight; measured ≤ 1.5 ms).
- Contract: fire-and-forget requests with latest-wins coalescing (at most one child in flight + one queued trailing request, so held volume keys cannot pile up subprocesses); a bounded per-request timeout (default 1000 ms, the historical budget) enforced off the UI thread; nonzero exit, spawn failure, timeout and unparseable output all leave the last known good cached state intact, log a diagnosable warning via `audio.mixer.runner`, and never wedge the queue. Results and state updates are marshalled back to the owner thread; shutdown kills any in-flight child and joins the worker with no leaks.
- Focused unit suite (`mixer-process-runner`, CTest) drives a fake delayed child executable through slow success, timeout, nonzero exit, garbage output, missing binary, rapid-fire coalescing, caller-thread timing and busy-shutdown cases — clean under ASan/UBSan with `detect_leaks=1`. No audio hardware needed.
- Backend behavior is unchanged in this step: the existing synchronous `amixer`/`wpctl` paths stay as they are until the follow-up cards convert them onto the seam.

### Added — CI: burst-branch RC lane for overnight validation
- `pi-rc.yml` now has a `workflow_dispatch` `mode` input: `build-only` (default), `rc`, and `restore`. `rc` builds a `burst/**` ref, deploys it to the Pi as an RC, and writes a `burst:` owner marker plus a saved beta restore rev. `restore` rebuilds that saved rev and clears the RC markers, or no-ops safely if beta has advanced since the RC went on.
- The PR lane and burst lane share a `pi-rc-deploy` concurrency group, so only one RC deploy can run at a time — the Pi never sees two RCs. A burst RC cannot overwrite another burst RC, and a PR close/restore will not mistake a burst marker for its own (`pr-N`).
- No public GitHub comments from the burst lane: evidence goes to the workflow job summary; the card carries the local handoff. `build-only` remains zero-Pi-contact and now validates its `ref` input.

### Fixed — CI: closing a pi-test PR unmerged now restores the Pi even when its RC marker matches
- The `pi-rc.yml` "Decide mode" step wrote `PR_NUM=pr-N` to `$GITHUB_ENV` and then compared the Pi's marker against `$PR_NUM` **in the same step**, where that name was still an unset shell variable (empty). A matching `pr-N` marker therefore always read as a mismatch, so closed-unmerged PRs #89 and #90 silently chose `MODE=skip` and left the RC build on the Pi despite a valid saved beta rev (issue #91).
- The decision is now a repo script (`scripts/pi-rc-decide.sh`) that computes `pr-N` from its argument, with the marker and saved rev passed in by the caller over SSH. The workflow runs that tested script and writes its output to `$GITHUB_ENV`; the marker read is also logged for diagnosis.
- `scripts/pi-rc-decide-test.sh` (registered as CTest `pi-rc-decide`, so both CI lanes run it) covers matching-marker restore, later-owner mismatch, missing rev, no marker, non-closed rc mode, and a stale `PR_NUM` in the environment. A stale env value can no longer produce a false skip.
- A second bug surfaced in the real-PR proof: restore treated "official marker still equals the saved rev" as "the Pi was never displaced" and skipped the redeploy. RC deploys deliberately never touch the official marker, so that fast path fired for every real RC build — it cleared the RC markers but left the RC binary installed. Restore now always redeploys the saved rev.
- The restore build stamps the binary with the saved rev, exactly as `ci.yml` stamps the beta build with `github.sha`, so the restored binary is byte-identical to official beta (md5 included) rather than carrying a `-pi-rc` version string.

### Added — RetroArch: the sort header is now a control in every list view
- In **Games** and **RECENTLY PLAYED**, pressing **Up** on the first letter of the letter strip moves focus to the `N GAMES · A-Z/Z-A` header instead of wrapping to the last letter. **Down** returns to the same highlighted letter, **Enter** toggles Game Sort and persists it through the normal setting, and only the direction word (`A-Z`/`Z-A`) highlights on focus — the `N GAMES ·` prefix stays plain text.
- **Playlists** gets the same control for its `playlist_sort` setting: **Up** from the first row focuses the `N PLAYLISTS · SYSTEM/COMPANY` header, **Enter** toggles and persists the sort, **Down** returns to the list, and only the direction word highlights. All RetroArch list views now share the same contract.

### Changed — RetroArch: Recently played now has full list-view parity
- **Game Sort (`A-Z` / `Z-A`) applies to RECENTLY PLAYED too**, using the same backend ordering as the Games screen. The stored list stays newest-first; only the view's order changes. `lastPlayedGame()` (one press from the main menu to the last game you launched) still reports true recency.
- **The header now reads `N GAMES · A-Z` / `N GAMES · Z-A`**, matching the Games screen, and the letter-strip buckets are sorted by the active direction (`#` leads A-Z, trails Z-A).
- **Changing direction keeps the selected game selected**, re-finding it by name after the re-sort, exactly like the Games list.
- Unit tests cover both sort directions and the recency-preserved one-press accessor; the offscreen smoke test checks the header, strip order and selection sync in both directions.

### Fixed — RetroArch: Recently played now matches the other game lists
- **Thumbnail preview restored on the RECENTLY PLAYED view**, using the same `GamePreview.qml` panel and `thumbnail_display` setting as the Games screen (LEFT / RIGHT / OFF). Thumbnails are resolved on demand from each stored entry's playlist basename, so moved ROM folders can't leave a cached stale path.
- **Letter-strip quick-scroll restored**, with `A`/`An`/`The` removed from bucketing like the games list. Buckets follow the view's newest-first display order (no alphabetical re-sort) so jumping to a letter lands on the row actually showing it. The games count header and BROWSE hint are shown too.
- Both zones use the Games screen geometry: preview and list never overlap the strip, and hiding thumbnails via `thumbnail_display=OFF` leaves the letter strip working.
- Offscreen smoke test (`recent-view-smoke`, 14 assertions over the real QML with stubbed backends) covers strip order, boxart resolution, row→strip sync, zone geometry and the OFF setting; full suite 16/16 green locally and on the x86 CI lane.
- The smoke test loads the real `AppBar` (MultiEffect), which needs `QtQuick.Effects` (Qt >= 6.5) and `QtQml.WorkerScript`. x86 CI's install-qt-action Qt 6.7 SDK has both and runs all 16 tests; the arm64 lane is Ubuntu 24.04's Qt 6.4, where `QtQuick.Effects` has no package at all. That lane still builds the smoke binary and runs the other 15 tests — QML layout is arch-independent, so the x86 lane is the correct place for it.
- Owner CRT report: recently played was reachable but the thumbnail and letter strip were missing. Promotion stays on hold until the owner re-checks the RC build.
- **A launch that never spawned no longer enters Recently played.** `QProcess::start()` is asynchronous, so recording immediately after it gave a missing RetroArch binary a RECENT slot — the exact case the old comment claimed to prevent. Recording now happens on `QProcess::started`; a regression test points the backend at a nonexistent binary and asserts the list stays empty (RED before the fix, GREEN after).

### Added — RetroArch: games sort direction and a game count on the Games screen
- **Game Sort setting** (`A-Z` default, `Z-A` alternative) for the Games list. Leading `A`/`An`/`The` are ignored, matching the letter buckets the quick-scroll strip already used; the strip reverses in `Z-A` so its order matches the rows.
- **The count and active direction show at the top right** of the Games screen (`5 GAMES · A-Z`), the same information the Playlists screen already showed per system.
- **Changing direction keeps the selected game selected.** Indices mean different rows after a re-sort, so the view re-finds the game by name and persists its new index — a later visit (or restart) still lands on that game.
- Unit tests cover boot defaults, live setting changes, article/case-insensitive ordering and stable ties; offscreen smoke checks both directions end to end, including the letter strip and header layout.

### Added — RetroArch: recently played, and your place in a playlist kept
- **RECENTLY PLAYED at the top of the playlist screen**, when there is anything to show: a list of the last twelve games across all systems, newest first, launching straight from stored entries without re-reading a playlist. Re-playing a game moves it to the front instead of adding a second copy. Stored in `retroarch_recent.json` beside the rest of the app's data (atomic write: a power cut leaves the previous list, not a truncated one; a corrupt file reads as empty and is repaired on the next launch).
- **Playlists remember where you stopped.** Backing out of a long list and coming back lands on the same row — including after a service restart, which in-session navigation state never covered; index 0 is stored as "no position", so the common case costs no write.
- The playlist screen restores its own selection by row identity rather than a bare index, so the row that appears when the recent list becomes non-empty (and disappears when it empties) no longer shifts what gets highlighted.
- Unit tests: `recent-games` (12 cases — dedupe/move-to-front, cap, corrupt and missing files, atomic round trip, position clamping) alongside the existing 15-test suite.

### Fixed — CI: an official deploy could skip over an RC build, and never cleared the RC markers
- **The skip fast path trusted a marker the RC lane never touches.** `pi-rc.yml` deploys un-merged PR heads and records them in `.240mp-rc-deployed-{rev,pr,md5}`; the official deploy gate only compared `.240mp-last-deployed-rev`. So "same commit already deployed" could be true while the Pi was actually running an RC build, and the deploy would skip — blessing an RC binary as the beta deploy.
- **Stale RC markers survived a real deploy.** Observed on the promotion of `55b129c`: the official deploy overwrote the official marker and left PR #77's `rc-deployed-*` files in place, so the Pi still read as "carrying an RC build" to every later probe (and the leftover `pr-77` owner makes a future PR close think it may need to restore).
- **Both paths fixed**: the `deploy-pi` job in `ci.yml` and `scripts/deploy-pi.sh` now skip only when the official marker matches AND no RC markers exist, and a verified deploy writes the official marker while clearing all four `.240mp-rc-*` files. `pi-rc.yml` already did this on its own official path; the official side had no equivalent.
- **All four RC markers are consulted, not just one.** The first cut compared `.240mp-rc-deployed-rev` alone, but `pi-rc.yml` writes `.240mp-rc-restore-rev` *before* the deployed trio, so an RC run aborted between those writes leaves only `restore-rev` — the Pi stays claimed by the RC lane and the gate skipped anyway. A lone `.240mp-rc-deployed-pr` was invisible for the same reason.
- **A failed marker read no longer reads as "clean".** Two separate `ssh` calls with `|| true` meant a read that died after the official marker matched produced a skip. Both paths now read the official marker and all four RC markers in one round-trip and require a trailing sentinel; anything less is "unknown" and deploys.

### Fixed — CI: the weekly Pi canary existed only on an unmerged branch
- **The canary never ran.** `t_b0f88741` added `schedule: - cron: '0 9 * * 1'` to `pi-probe.yml` on `probe/pi-canary`, but that branch was never merged and a scheduled workflow only fires from the repo's **default branch** (`dev`). `dev` still carried the original 1,254-byte one-shot probe: no `schedule`, no audio job. Weekly deploy-drift and audio-grant monitoring was a no-op.
- **One merged workflow now:** `dev`'s `pi-probe.yml` carries `workflow_dispatch` (with the `force_schedule` proving aid), the FACT 1–7 audio ground-truth job inherited from `probe/pi-audio-facts`, and the Monday 09:00 UTC cron. The audio job runs on both `workflow_dispatch` and `schedule`, with `CANARY_MODE=1` guarding every write (FACT 4's `amixer sset` and the sudo attenuation block) so a cron run can never move the volume under a viewer.
- **Caveats kept in the file:** `schedule` only fires from the default branch, and GitHub disables scheduled workflows after ~60 days of repository inactivity.

### Fixed — Display hand-off: stop the renderer instead of letting it draw into a dead DRM fd
- **The app kept rendering for the whole session after handing the screen to a child.** The hand-off switches VT, which is supposed to suspend Qt's render thread through the kernel's VT signal — but on this Pi the app holds no console fd (only `/dev/dri/card1`), so Qt EGLFS never installed a VT handler and never stops drawing. The switch itself succeeds, so nothing warned: the renderer simply kept submitting atomic commits to a DRM device it no longer owned, at 60 Hz bursts. Measured on beta: 1573 `Failed to commit atomic request (code=-13)` lines across three RetroArch launches, ~10% CPU held while the game ran.
- **Fix**: `DisplayHandoff` now emits `displayTaken()` when the hand-off completes, and the root window is hidden on it — EGLFS renders nothing while hidden — then shown again on `displayReturned()`. The CRTC state is still captured before the hide, so the post-exit restore is unchanged. `MP240_HANDOFF_KEEP_WINDOW=1` restores the old behaviour for A/B testing on hardware.
- **The way back had to be sequenced too.** Hiding the window means EGLFS re-initialises when it returns, and doing that in view is a visible flicker on the CRT. The window is now shown while the display is still parked on the child's VT, and the VT switch back waits for `QQuickWindow::frameSwapped` — a frame that provably reached the display — with a timer as the safety net so no frame can strand the Pi on the child's VT.
- **One modeset on the way back, not two.** The legacy `drmModeSetCrtc` restore existed because an atomic cleanup leaves the CRTC disabled and a live EGLFS then gets EINVAL on its next flip. When the window is hidden for the hand-off, showing it re-initialises EGLFS, which programs the CRTC and mode itself — so the restore became a second, redundant modeset, and each one blanks a composite output. It is now skipped in that world and kept for `MP240_HANDOFF_KEEP_WINDOW=1`, where EGLFS really does stay alive.

### Changed — App-wide volume: one level, two renderers (#34)
- **Volume keys work everywhere**: volume up/down/mute used to be each module's private business (weather music, mpv only). `InputManager` now hands them to a single app-wide `VolumeManager`, so pressing the remote's volume keys on any screen — menu, Plex, YouTube, channels, a game — changes the same level, and RetroArch keeps doing its own thing while a game owns the screen.
- **The system mixer is the truth**: `VolumeManager` reads and writes the ALSA mixer (via `amixer`), so the number in the bar is the number the speakers actually play at. mpv and the weather music player run pinned at 100 and no longer attenuate on their own; the only exception is the speech duck, which is transient by design.
- **One OSD, two renderers**: while the app owns the screen, a QML `VolumeBar` draws the level on every view — menu, module, guide, wherever you are — whether or not anything is currently playing. While mpv owns the screen — music, video — the app renderer is suspended, so the bundled `mpv-media-keys.lua` draws the same level through `script-message 240mp-volume-osd`, fed from the same `VolumeManager` state. One level, whichever surface happens to be visible.
- **The level survives a restart**: it is persisted to `volume.json` in the app data dir and restored at startup, muting included.

### Fixed — CI: the Pi deploy trigger that could never fire
- **Beta pushes never reached the Pi.** `deploy-pi.yml` was triggered by `on.workflow_run: workflows: ["CI"], branches: [beta]` — and `workflow_run` only emits for runs of the triggering workflow on the repository's **default branch**, which is `dev`. Beta had 17 green `CI` push runs and produced **zero** deploy runs; the last Pi deploy (`43388847788`) came from a `dev` push, which the `head_branch == 'beta'` gate then rejected. Net effect: the Pi received deploys from no branch at all.
- **One mechanism now, in one file:** the deploy is a job in `ci.yml` — `deploy-pi`, `needs: [build-linux-arm64]`, `if: github.ref == 'refs/heads/beta'` — so "deploy only after a green build" comes from the job DAG instead of an event that never arrives, and the standalone workflow is gone. Under a push trigger `github.sha` is the beta tip, so the existing `~/.240mp-last-deployed-rev` fast path and the tarball-md5 verify work unchanged.
- **Docs and comments repointed** at the deploy's new home (`.hermes.md`, `scripts/deploy-pi.sh`). `paths-ignore: ['**.md','**.yml']` still means a docs/YAML-only promotion queues no CI and therefore no deploy — intended, not a regression; re-deploy an unchanged beta tip with `workflow_dispatch`.

### Fixed — Virtual channels: Guide video previews respect media-server transcode settings & stay smooth
- **Plex / Jellyfin / Emby Guide previews**: `preview_stream()` now passes `m_previewPosition` and `transcodeAllowed = true` and honors `PlexBackend`'s resolution-aware `forceTranscode` decision (only transcoding items above the configured Video Quality threshold, starting at the live offset), while cleaning up active preview transcode sessions on channel change (`cancel_preview_stream`).
- **Non-blocking preview teardown & real-time pacing**: replaced synchronous `waitForFinished()` in `stop_preview_video()` with async process teardown + per-launch `/dev/shm` directories (eliminating Guide navigation freezes), and moved `fps=8` before `scale=320:240` with `realtime` pacing when muted (`--no-audio`) so `mpv --vo=image` no longer runs untimed at 300%+ CPU.

### Changed — Virtual channels: streamlined settings & couch navigation
- **Settings → CHANNELS simplified**: trimmed `manifest.json` down to 7 couch-essential rows: `Enabled`, `Manage Channels`, `Media Folder`, `Weather Channel`, `Auto-Scroll Guide` (simple ON/OFF toggle, replacing the 5-state idle menu), `Channel Preview`, and `Guide Music`. Cut background technical knobs (`Schedule Ahead`, `Scan Library at Start`, section headers).
- **Manage Channels / ChannelSources cleaned up**: streamlined per-channel settings down to core essentials (`Source`, `Type` on custom channels, `Shows` / `Films`, `Order`, `Logo`, `Rebuild`, `Seed From Recipe`, `Rename`, `Delete`).
- **New couch-friendly Order modes**:
  - **TV channels**: `Rotating` (shows take turns in episode order), `Shuffle` (random across all shows), `By Airdate` (chronological by original airdate), and `Marathon` (play each series in full before moving to the next).
  - **Movie channels**: `Shuffle` and `By Release Year` (chronological by release date).
- **Deadweight shed**: deleted 11 unused/abandoned complex desktop scheduler QML subviews (`PlanEdit`, `BlockEdit`, `Bookings`, `BookingEdit`, `BookingDays`, `Interstitials`, `SourceIdents`, `IdentLevels`, `BannerSettings`, `PoolEditor`, `SourcePick`).

### Added — Virtual channels: bundled preset network & studio logos (Unit D)
- **Auto-branded preset channels**: creating a TV Network or Movie Studio preset channel now automatically assigns a matching corner logo (`set_channel_logo` / `preset_logo_for_subject`), using a normalized alias table that maps `library.db` studio/network variants (e.g. `Warner Bros. Television` → `warner-bros.png`, `Williams Street` → `adult-swim.png`) to canonical marks.
- **38 monochrome white/transparent PNG logos** (`modules/virtual_channels/assets/logos/*.png`), rendered from Wikimedia Commons Public Domain SVGs and attributed in `THANKS.md` (covering major US TV networks, animation/cable brands, and movie studios present in `library.db`).
- **Seamless user override**: `list_logos()` and `logo_path()` resolve `<dataRoot>/logos` first and fall back to the bundled `assets/logos/` directory, so user-dropped files override bundled marks by filename and bundled marks remain selectable in `LogoPicker`.
- Test surface: new `testPresetLogos()` in `VirtualChannelsBackendTest` verifying bundled fallback, user override precedence, deduplicated `list_logos()`, alias normalization, and `create_preset_channel()` auto-branding (network/studio only, never Genre). Full suite **14/14 green**.

### Added — CHANNELS: the dial reads its shelves
- **Local library index + launch walk**: the module looks its shelves over once at start ("Scan Library at Start", on by default in the new Shelves section), so the add-on lists offer only what is actually watchable — ordered by how many titles carry each category, count beside every row ("12 shows") — instead of an alphabetic catalog list of things nobody owns. A fresh app, an un-scanned app, and a viewer who turns it off keep the old lists byte for byte. A server that stays offline keeps its last-known lineup instead of darkening its channels; boxset shelves contribute their real counts; a genre nobody has stops being offered rather than sitting at a stale count.
- **Network questions the shelves answer skip TheTVDB**: a show the walk saw a network for answers from the catalog's own network listing, so a seed pays zero searches for it — cold or warm, across boots. Answers TheTVDB does provide are taught back into the index, so a seed never re-pays a question it already asked.
- **Channel settings declutter**: the Weather Channel row and the guide's Weather Music row only appear while the weather module is on — a control for something the viewer cannot turn on is clutter, not a choice.
- Built on Qt's Sql module (optional): a build without it compiles the index's unavailable-shim half and keeps the catalog path, so the index can never break a platform's build — the Pi ships it (Qt 6.8's Sql is packaged on Debian 13) and dev/CI use 6.10's.

### Fixed — Virtual channels: the index stops remembering what the shelves forgot
- **A show you deleted from a source now leaves the lineup instead of lingering forever.** The index merges tallies per (source, channel) pair: when a source is walked, its own rows follow the walk — a show the walk didn't mention drops, so a genre nobody has anymore stops being offered and the counts beside the option rows stop lying. A source that stayed dark this walk keeps its last known lineup, so one offline server can never darken a channel. A show's row drops only when **every** source that walked over it has stopped mentioning it — sources that never walked keep their shows standing through partial walks. Unit-pinned in the lab (`LibraryDbTest`: walked-unmentioned drops, dark-source kept through a Plex-only walk, dropped once Emby walked over it too).

### Fixed — Virtual channels: a genre recipe airs the shows that carry the genre

- **TV genre recipe swept the MOVIE library and kept airing "Private Snafu"** (owner report: the genre channel always landed on the same theatrical short). Both `readPools` sites forced `wants=Films` for any genre recipe — the plan-block `Draws::Genre` case and the `genre` pool-entry kind — so a TV channel asked the server for films in that genre and whatever shorts happened to carry it won. Now `Films` is forced only when `isMovieChannel(channel)`, mirroring the Collection twin; a TV genre keeps its `Episodes` scan.
- **The enumeration then ignored the genre entirely**: the genre lives on the **series** item, not on its episodes, so an episode scan that never consults show-level genres can't narrow at all. `MediaServerSource` with a genre filter now asks `load_items` for `Series,Episode` beside the flat episode scan (same shape the Emby children call already sends as `Season,Episode`), and `GenItems` first collects the series whose `genres` intersect the request, then keeps **every** episode of a matched show — whole series in, never episode-by-episode cherry-picking — OR-ing with the existing `match`/`showIds` roads.
- Test surface: new `MediaServerSourceTest` section "a genre enumerates whole shows, not episodes" (fake show rows carry the genres; the Comedy show's two episodes are all in, the Documentary show stays out, and the call log pins `load_items(tv1,Series,Episode)`) plus new `VirtualChannelsBackendTest` "genre recipe wants films only on movie channels" (block AND entry × tv/movies kinds, 4 wants asserts). Full vchan suite **1458/1458 green** in the lab.

### Fixed — Virtual channels: preset channels read their recipe kind as Genre (whole-library presets, self-destructing re-seeds, resolver false matches)
- **"Horror aired nothing, ABC aired everything, collections were luck" — one trap**: presets stored their recipe kind as the JSON *string* `"1"`, and `QJsonValue::toInt()` on a JSON **string returns 0**. Every Network (kind 1) and Collection (kind 2) preset therefore re-typed into **Genre** on the next reload: the TheTVDB identity filter never ran (journal proved it — 21,615 tried, **0 refused**, zero searches paid), a bogus `genres` block rode into the source blocks, and the ABC preset harvested the **entire library** into 259 picks. Genre channels only survived by coincidence of the same 0. Fix: `create_preset_channel` records kind as a **number**, and every read goes through the new `presetKindOf()`, which still accepts the legacy string generation so channels already on disk re-seed correctly before they are rewritten.
- **Re-seeding a preset channel zeroed it** (Horror: 61 picks → re-seed → 0). The seed merge's un-tick inference scanned the recipe's *own prior picks* — which the merge had just removed to replace them — and concluded the viewer had un-ticked every one of them, pushing the whole pick set into `preset.off`. The un-tick memory the UI already maintains authoritatively in `preset.off` (toggle remembers, re-tick forgets) is honoured alone; the inferential pass is gone.
- **The resolver adopted unrelated first hits**: `bestMatchIndex` fell through to `result[0]` when nothing matched, so every show missing from TheTVDB inherited a stranger's network identity — with Private Snafu resolving to ABC's network id, *nothing* ever refused and the whole library passed. Scores that match nothing are now rejected outright (`-1`), which is also what makes the identity filter meaningful.
- **Added — TheTVDB network-id cache persists across boots**: the resolver gained `loadCache()`/`saveCache()` over `<dataRoot>/tvdb-network-cache.json`; seeds load it at resolver construction and save it at harvest end. Network seeds no longer re-pay one TheTVDB lookup per title per boot — the dial's lookups are stable, and building channels gets faster with every session (owner's "cache of relevant results").
- Test surface: preset-smoke's kind assertion moved to `presetKindOf` (the number contract), new cases pin **string-kind → toInt == 0 (the trap itself)**, `presetKindOf` on both generations, `bestMatchIndex` refusing unrelated hits, and the cache round-tripping through a file with zero network. Full vchan suite **1430/1430 green** in the lab.
- Also this push: `scripts/install.sh` makes the Pi's `ExecStopPost` handler restart-safe — the unit now passes `%1` (systemd's stop command: `stop|restart|kill|empty` on app exit) and `240mp-stop` keeps the Pi powered on `restart`/`kill` instead of powering off mid-restart, so dev/agent service restarts no longer reboot the box. Explicit `stop` and app exit codes keep the old semantics.

### Fixed — Module-settings channel entry no longer greys out on bare view hops (live Pi, 2026-11-11)
- Editing a channel reached through **Manage Channels** greyed out the moment a channel was chosen. Pi journal said it outright: `navigate to=ChannelSources.qml` → `file:///opt/240mp/share/240mp/ChannelSources.qml: No such file`. Cause: Settings' module submenus open the module view **app-level** (Main.qml's Loader loads `Manage.qml` directly), so Manage.qml's bare `navigateTo("ChannelSources.qml")`/`("PresetRecipe.qml")`/`("TextEntry.qml")` hops land in Main.qml's handler, where Qt's Loader resolves bare tokens against the **share root**, not the module's views dir. In-module hops were always fine — Root.qml's bridge resolves them relative to `views/`; that convention simply wasn't honoured when the hop skipped the bridge.
- Fix at the app-level choke point: Main.qml's `navigateTo` resolves bare `*.qml` tokens against the **last-loaded view's URL** via a new `AppCore::current_view_source()` getter (QString — dodging Qt.resolvedUrl's QUrl→QObject* marshalling trap and Qt6's dropped `left()`); tokens already carrying a directory ("views/", "modules/") keep resolving share-root-relative, so every existing hop is untouched.
- Lab-proven on the exact repro walk (ModuleList → Settings → CHANNELS → Manage Channels → add-preset row): the hop now emits the resolved file URL, `PresetRecipe.qml` opens clean and the kind ask starts — zero QML errors. Bundled on the same head: Guide's crawl Enter fix (highlight index modded **before** the row guard; during the crawl the model is `rows.length * copies`, and a residue index past the rows could outlive the crawl and swallow Enter on the selected row).
- pi-rc evidence: PR head `772a53b` → Pi binary md5 matches its tarball, service active. Awaiting owner CRT word on the Manage-ABC edit flow before done.

### Added — Virtual channels: preset channels (stand up a whole channel from a category)
- A viewer can now build an entire channel from a category instead of one folder or series: `Manage.qml` gains an "Add Preset Channel" row on the dial, leading a two-step picker (`PresetRecipe` → `PresetSubject`). Pick a recipe (by genre, by network, or by collection), then the specific subject; selecting one calls `VirtualChannelsBackend::create_preset_channel()` and drops onto `ChannelSources`, exactly where a manually-added channel lands. The channel's identity is derived from the subject (a "by network" preset for AMC becomes the "AMC" channel); the recipe is recorded under the channel's `preset` key so a future generator can rebuild it.
- **A preset stands its content up on the channel at creation** — the whole point, so new channel creation is a pick instead of a hand-build. The flow asks what the channel airs (TV shows or movies) before the category, so the recipe looks for content among the right kind from the start. Selecting a subject calls `create_preset_channel()` and then `seed_preset_channel()`: the recipe runs through the generation machinery against **every** connected source (Plex/Jellyfin/Emby; a network recipe with no server-side filter scans broad and keeps only what TheTVDB resolves to that network), the picks are deduped by title and written into the channel's `programmes` pools as concrete series/film entries marked with the recipe. The viewer lands on `ChannelSources` with **everything already ticked** — the Series/Films browser shows the library with the picks checked, and removing one show is one press. The recipe's block fields are retired once the picks are written, so the recipe never decides what airs on its own; `preset.off` remembers what the viewer took out, and the new **Seed From Recipe** row (`N OFF`) re-runs the recipe and adds what the library has gained since without putting those back. A recipe the library cannot fill creates nothing — the subject picker answers with the reason instead of parking an empty channel on the dial.
- **Movie presets ask by studio, and every server carries it**: on a movie channel the "by network" recipe slot reads as *studio* — the subject list is the connected library itself (browse the films, tally the studio strings), and the pick rides into Plex, Jellyfin and Emby blocks as a `studios` server-side filter (Plex's `Studio` attribute, Jellyfin's `Studios` array, Emby's `Studios` with a bare-`Studio` fallback for older servers). Only TV networks keep the TheTVDB resolver; a studio channel needs no apikey. The picker screens follow the mode: the recipe row relabels per kind ask, the subject picker offers only studios, `ChannelSources` grows a Studios row only on movie channels that carry the filter, and a local-files channel refuses the studio picker outright.
- Backend: new `Catalog` (`src/modules/virtual_channels/Catalog.{h,cpp}`) is the TheTVDB subject-matter source (`parseGenres` / `parseNetworks`) and emits the `[{id,label}]` shape the QML picker expects. `VirtualChannelsBackend` gains a lazily-built TheTVDB resolver (constructed once a stored apikey is known), `load_catalog()` for genre/network (TheTVDB) and collection (rides the connected media-server browse), and `catalogReady` / `catalogFailed` delivery. `TVDBNetworkResolver::listGenres()` / `listNetworks()` now delegate to `Catalog`.
- Test surface: `CatalogTest` + a preset-channel case in `VirtualChannelsBackendTest` (pure logic). A standalone offscreen smoke test loads both views with stubbed backends and asserts 3 recipe rows, the 2-row kind ask opening the screen, a 2-row catalog populate, `create_preset_channel(0,"389","AMC","tv")`, that activating also calls `seed_preset_channel()` on the channel it just made, and that the seeded channel navigates to `ChannelSources` — proves the wiring and data flow, not pixel layout. Full vchan suite green; CI arm64 + macOS green.

### Fixed — Virtual channels: network presets no longer return an empty catalog (issue #64)
- `Catalog::parseNetworks` matched the v4 company type against the literal `"network"`, but the API returns it capitalised (`"Network"`), so **every** network was filtered out and a "By Network" preset listed nothing. The compare is now case-insensitive, and `CatalogTest` covers the real-world `"Network"` casing so it cannot silently regress. Genre and network catalogs now load live with the stored TheTVDB v4 key (35 genres / 500 companies verified against the live API); the key lives in `<dataRoot>/thetvdb_apikey` (0600).
- **A seeded genre channel swept the whole library in**: the seed pass writes its recipe as a `genres` filter in the source block, but `readPools` never read that field back, so the harvest job asked for nothing and aired everything the section held. Block reads now include `genres` (and the new `studios`), `bookingWants` gates studio strings case-blind, and `channel_source_config` reports the block's filter among the channel's genres — the builder screen used to answer NONE under a channel that had named the genre when it was built.

### Fixed — Weather location reads no longer fail silently outside the module scope (issue #53)
- `views/ModuleSettings.qml:141` and `views/LocationPicker.qml:31-32` called `weatherBackend.primaryLocationLine()` / `.looksLikeCoordinates()` directly, but `weatherBackend` is a **module-scope** context property that is null outside the weather module's QQml tree — so the app-level views read a null receiver and got empty strings, silently. Root cause was deeper: the proxy in `AppCore` used `Q_RETURN_ARG(QVariant, ret)` against `WeatherBackend`'s `Q_INVOKABLE`s, and `QMetaMethod::invokeMethod` logs a "return type mismatch" and returns empty — `QVariant` never matched the `QString`/`bool` signatures.
- Fix: `AppCore` gains `weatherPrimaryLocationLine()` / `weatherLooksLikeCoordinates()` (`Q_INVOKABLE`, cross-scope via `QMetaObject::invokeMethod` on the `com.240mp.weather` backend in `m_backends`), and the two views call those instead. Return types now match exactly (`Q_RETURN_ARG(QString, ret)` / `Q_RETURN_ARG(bool, ret)`).
- Test surface: new `src/tests/AppCoreWeatherProxyTest.cpp` (6 checks) exercises the proxy directly under `QT_QPA_PLATFORM=offscreen` — no QML engine, no network. Full suite 12/12, app builds 0 warnings under `-Wall -Wextra`.

### Fixed — TVDB network resolver no longer double-frees its network manager (issue #32 production-readiness review)
- `TVDBNetworkResolver` allocated its `QNetworkAccessManager` with `new QNetworkAccessManager(parent)` yet deleted it in the destructor — a non-null parent (the future backend owner) would **double-free** on destroy. The resolver isn't a `QObject`; the parent was only ever for QNAM, so it was dead weight that turned into a crash once the resolver is wired into `VirtualChannelsBackend`.
- Fix: drop the `parent` constructor param, make `m_nam` a value member (RAII, no manual delete, no double-free possible), reorder members to clear `-Wreorder`. Added a construct/destroy lifecycle test that would have caught the double-free.
- Verified ASan-clean (0 memory errors); the `invalid nullptr` connect warnings are pre-existing Qt-internal on a headless QNAM — a bare `QNetworkAccessManager` reproduces them, so they're not from this code. Committed `a948dac`.

### Added — App-wide system-volume mixer abstraction (issue #34 spike)
- New `src/audio/` `VolumeMixer` interface + factory so every module can route volume through one source of truth (the #34 goal of "consistent across the board"). Factory picks at runtime: **PipeWire** (`wpctl`) > **ALSA** (`amixer`) > **no-op stub**, so the app never crashes on a box with no audio control. No real hardware needed to test — the factory falls back to no-op on WSL/CI.
- `VolumeMixer`: 0..100 level + separate mute (mute survives level changes, matching a physical button). `PipeWireVolumeMixer`: `wpctl set-volume`/`set-mute`, float-fraction spec. `ALSAVolumeMixer`: `amixer sset`/`get`, parses the echoed state. `NoopVolumeMixer`: succeeds but changes nothing. `VolumeMixerFactory`: `findExecutable`-based detection.
- CMake: audio TUs added to the app; new `volume-mixer-tests` target (7 checks, all pass). Full suite 13/13, app builds 0 warnings under `-Wall -Wextra`. **Not yet wired into any module** — next step is the central `VolumeManager` that owns one of these and feeds the OSD + player + weather mpv.

### Changed — Channel settings wall consolidated (two sections; the 18-row wall gone)
- `modules/virtual_channels/manifest.json` settings now use the core `Settings.qml` section-row contract: **DIAL** (media folder, weather channel, schedule-ahead) and **GUIDE** (single idle row, preview, weather music) — 18 selectable rows → 8. Removed rows: volume + `normalize_volume` (media keys already own volume), `audio.language`/`subtitles.*` (per-title player choice), `tuning_screen`/`banner`/`logo` looks (channel identity is auto-branded from its subject — presets take that off the user), and `rebuild_all` (schedules already top up at boot and 04:00 — that row *was* the visible bug in the original report).
- Merged: `guide.autoscroll` + `guide.resume_seconds` → one `guide.idling` row (`Keeps Crawling | Holds · 30s/1m/5m/Never`); `Guide.qml` prefers the merged row (manifest schema default answers first) and keeps the legacy pair as a safety net. The resolved mode is logged to the debug state channel: lab offscreen drive saved `"guide": {"idling": "Holds · 1m"}` through the real UI and re-entry emitted `guide-idle auto=on resume=60s src=setting`; boot read of the schema default emitted `auto=on resume=5s`.
- `views/ModuleSettings.qml` gains the section-row rendering (mirrors core Settings, which already skipped sections — that asymmetry was the user-visible half of the original complaint) and `snapSelectable(idx, dir)`: the row stream in the drive read `Enabled → Manage → Media Folder → … → While Idle` with section indexes never landed on or stopped at, both directions.
- Screenshot artifact of the settings screen: objective pixel probe shows exactly 10 text-row bands in expected order with the selected row's full-height accent distinct; pixel-level visual truth waits for Ian's CRT gate. Naming note for the merge: fork settings keys stay `auto_fit`/`autoFitEnabled()` — upstream's `auto_crop` rename never comes into this fork.

### Added — Virtual channels: the library becomes a TV schedule (upstream's module merged to dev)
- `modules/virtual_channels` (schedule parsing that drops rather than crashes, pool generation across interstitials/bumpers/ident cards/rotations, guide grid, channel dial) merged onto dev via PR #58; enabled in the default `settings.json` (`modules/virtual-channels`, `virtual-channels.settings`).
- Integration constraints upstream never had to know: our number-collision rule (a virtual channel claiming a built-in's number **re-homes the built-in**, never the viewer's setting — unit case: "the weather channel shared number 1; moved to 2") and the `osd-channel` gate (switching a channel off frees its number **exactly** = its configured position, unit case: "off numbers the dial has taken").
- Test surface: `t2test` target `vchan-tests` — 1311 checks, 0 failures at dev tip (schedule tolerance, pool plans, guide slivers, tuner joins, subtitle/audio track selection, holds across surfing, weather/guide re-homing).
- `InputManager` accepts `channel_up`/`channel_down` as debug-key tokens (synthesized to `Key_PageUp/Key_PageDown`); lab drive through the offscreen key channel proves the whole zap artifact: guide rows built from a fixture schedule (`rows=3`), `tuneFromGuide(±1)` reaches the Guide key handler, the Player opens and the channel's ident banner lands (log across the zap sequence: `channel banner queued: 0` then `1`). Wave evidence trail: PR #62 (label `pi-test` = RC deploy of the PR head on the Pi; official beta rev stays put).
- Channel previews degrade without QtMultimedia: "no video preview available; showing the still instead" is a handled path, not an error wall (the qml module warning itself is a lab-desktop build gap; the Pi's module set gets checked in the PR's RC evidence, and the still fallback works either way).
- The remote's Channel Up/Down keys were delivered-but-dead: `InputManager` synthesized `Qt.Key_ChannelUp`, a key undefined in QML, so the event could never match any view handler. Synthesis now emits `PageUp/PageDown` — the alias every channel view already accepts. Lab offscreen full-drive proof (green pre-push): guide↔channel zaps in **both** directions (`nav-internal to=Player stack=2` at both ends), ChannelUp from inside a channel returning to the guide, dial Up/Down walking the wrap `0→2→1→0` with `select idx=` at every step, and a full unwind at every stack level with zero skew. Same drive instrumented the CHANNELS module root (the only module root without `nav-internal` logging — RetroArch precedent) and moved the Guide dial to the core `RowTrackingListView` primitive. Wave evidence trail: PR #63 (`pi-test` label = RC deploy of the PR head; beta rev stays put).

### Fixed — All palette role pairs now clear the WCAG AA contrast floor (was: 12 palettes failed)
- A mechanical walk over every view (fill-bearing items with Text descendants; conditional branches paired by identical conditions; translucent fills and dead code skipped) yields exactly five rendered text/fill role pairs — `tertiary/primary/secondary` on `surface`, `surface` on `accent`, `accent` on `surface`. Audited across all 17 palettes: **12 failed the WCAG AA floor 4.5** (worst: Solarized Dark accent text 2.68, Monokai red header text 2.93, SMPTE indigo labels 2.96, late-night grays 3.42–3.86, T-120 secondary 3.45). Video 1 (4.33), Dracula (4.10) and the two Game-Boy palettes round it out.
- Fix: each failing role value moves minimally **along its own published hue** (constant hue and saturation, lightness shifted toward the AA floor; saturation only reduced when no lightness alone reaches it) — published spec hues stay recognizable, values stop being unreadable. 18 role values changed across 12 palettes; every changed line carries an inline `// was #XXXXXX …` derivation note. `Amber` and the five added palettes already passed untouched.
- Contract is now enforced where the table is parsed: `src/util/tests/ColorSchemesTest.cpp` gains a `renderedRolePairsClearAaFloor` test (WCAG luminance math inline, pair list hardcoded from the walk) — it fails red on the old table and green on the fixed one, both runs verified on the lab machine before any push. Audit tool (same table, pair counts per file): `/home/ian/src/vibes/theme_audit.py`.

### Fixed — Empty playlists settle with a visible empty-state message (spinner was permanent)
- `modules/retroarch/views/Games.qml` gated its LOADING spinner on `!Component.onCompletedCalled` — a property that **was never defined anywhere in the app** (qmllint: `Member "onCompletedCalled" not found on QQmlComponentAttached`; repo grep zero hits). Reading an undefined property is silently falsy forever, so any playlist with 0 games stayed LOADING indefinitely and the companion "No games in this playlist" Column (gated on `!spinner.visible`) was unreachable. Same pattern shadowed in `_base_games`/`_base_playlists`.
- Fix: real root-level `property bool listLoaded` set in the view's `Component.onCompleted` after the model assignment; spinner and empty-state text now branch on it. Lab-proven by screenshot band-analysis on an empty playlist (purple spinner box → gray text line, pixel counts objective), pure-QML change (no new unit surface); qmllint confirms `listLoaded` resolves and no `onCompletedCalled` member warning remains (only pre-existing unresolved-import noise).

### Added — Pre-merge PR verification on the Pi (`pi-test` label + `pi-rc.yml`)
- A dev→beta PR labeled `pi-test` gets its PR-head build RC-deployed to the Pi (rebuild on the arm64 runner, `240mp-ci-deploy` dance, service+md5 verify) with an evidence comment (marker, md5, boot state-log) posted on the PR. Beta stays the Pi's daily driver otherwise: unlabeled PRs never touch it. Closing such a PR without merging auto-restores the previously deployed beta build (one extra reboot). Ian's approving review + merge stay manual; the beta-push deploy remains the only official deploy.

### Fixed — Module roots survive shutdown teardown (null-appCore re-eval, was a silent TypeError)
- At process teardown the QML tree re-evaluates view property bindings while context
  properties are already nulled: `modules/{plex,local_files,retroarch}/views/Root.qml:14`
  ran `appCore.get_module_info(...)` against a null `appCore` during shutdown — noisy
  console TypeError on every quit, invisible live but polluting runlogs/diagnostics.
  Now the established guard parity all other module roots already use:
  `appCore ? appCore.get_module_info(moduleId) : ({})` (nfc/weather/scripts/emby/
  ambient/youtube roots already carry it; these three predate the pattern).

### Fixed — YouTube warm opens self-heal when a clock-fresh cached URL still 403s (warm-403 cure)
- A cached googlevideo URL can pass the `expire=` clock check yet be rejected 403 on
  replay: the signature binds to the minting session **and IP**, so `urlFresh()` is not a
  playback oracle. A warm launch that dies before the first position event now evicts that
  cache entry (`invalidateCachedUrl`, exact `videoId|format` key) and re-enters the cold
  resolve once, minting a URL bound to THIS session. One reactive retry per open
  (`lastOpenWasWarm`/`retryWarm403`, reset by user retry/resume/playlist advance) so a
  genuinely dead yt-dlp still ends at its message instead of looping. New pure-logic test
  target `yt-url-invalidate` pins eviction semantics: exact-key removal, format isolation,
  idempotent miss, no resurrection through prune.

### Fixed — SponsorBlock settings subview opens clean (QML contract, was a 4-error cascade)
- `SponsorSettings.qml` lacked the mandatory file-scoped `property var navParams: ({})`: every
  open of the subview fired `Main.qml` "Cannot assign to non-existent property", a
  ReferenceError poisoning `moduleId`, and a null-receiver `AppBar{}` icon binding — four
  QML error lines per open, silently. Now mirrors the sibling-view pattern (all 15 other
  views declare it; SignIn's `_selfInfo` file-scope snapshot feeds the AppBar icon, since
  context props like `appCore` aren't visible inside imported-component bindings).
  Lab drive of the full Settings→YouTube→SponsorBlock walk: 4 errors → 0, clean stack return.

### Changed — YouTube credential ladder sheds upstream's file-fed last stand (legacy axe burst)
- `CredentialMode` loses the legacy mode: `FileFallback` → `SignedOut`. With neither
  a pasted cookie nor a live device-flow chain, the account surface now shows **nothing**
  and the views point at pairing — upstream's file-fed display is gone from the ladder.
  `youtube_subscriptions.txt` stays as a **pre-pairing roster seed** (bootstrap channels
  before pairing), and legacy pos-only resume entries stay tolerated (save-file compat,
  a different axis). The unit test `ladderLastStand` became `ladderSignedOutNoLastStand`.
- `Items.qml` cold-start text leads with pairing ("PAIR YOUTUBE IN SIGN-IN SETTINGS")
  and offers the two txt files as the fallback bootstrap, not the other way round.
- Roster dead-end retry comment made honest: it retries on the SEEDED roster, not a
  credential-mode file path.

### Changed — Deploy-to-Pi gate: only a genuine push into beta deploys
- The deploy job gains `if: github.event.workflow_run.event == 'push'`. Trigger-level
  `branches: [beta]` alone also matches PR-triggered CI completions whose payload
  `head_branch` is the **PR head** (observed with PR #51 headed on beta) — those could
  deploy a pre-merge sha. A merge into beta is a push event and still deploys ✓.

### Fixed — YouTube cold-play waits end at once when the resolve already failed (#28 polish wave)
- Every case where the background resolve **completes but stores nothing** — dead network,
  bot-gated 403, ytdl spawn failure, no yt-dlp binary, entry already expired at store time —
  left the Player waiting out its full 15 s cap, and only then did mpv's `ytdl_hook` pay a
  second full resolve: worst cold path ~30 s. `playbackResolveDone(videoId)` now ends that
  wait at once, and `Player.qml` routes all three cold failures (timeout / empty resolve /
  stale pair) through one `takeYtdlFallback(reason)` path instead of one inlined copy.
- The URL-cache **store gate now runs the same pure `urlFresh` freshness check** (same
  120 s margin) as the read side: short-window entries stop being stored-but-never-served
  dead weight, and the ready-signal dead-end state can no longer happen.
- A transient SponsorBlock failure now **emits `sponsorReady` on the failure path** —
  the next playback retries instead of the Player waiting out the full 6 s ceiling.
  Failures stay uncached.
- Hygiene in the same touch: three `%d`-vs-`qsizetype` format sites in `YouTubeBackend.cpp`
  made exact, so the TU compiles warning-free on the CI lanes.
Gates: unit suites green (yt-url-cache 7/7, yt-sponsor-edl 10/10), TU compiles clean; the
runtime behavior lands with the Pi test.

### Changed — macOS CI cache lane warm + per-TU APP_VERSION define
- macOS CI re-ran from zero on every identity re-push and re-queued PR build — the chronic
  capacity bottleneck. Two-part fix, one deploy:
- `CMakeLists.txt`: `-DAPP_VERSION` moved from a target-wide define (which put it on every
  TU's compile line, so ccache hits were zero by construction) to a per-SOURCE define on
  the only two TUs that use it (`main.cpp`, `WeatherBackend.cpp`). Verified on a fresh
  Ninja configure: exactly those two object rules carry the flag, both compile green;
  test targets touch neither file.
- `ci-macos.yml`: `actions/cache` restore@v6 + save@v6 split so the store step sits
  directly after Build (a test failure still banks the compile cache); `CCACHE_DIR` under
  `RUNNER_TEMP`; key = event+ref with prefix fallback. ccache hashes inputs, so
  cross-commit reuse is always safe.

### Fixed — YouTube review pair: sign-in cookie row and OSC info garbage
- The SignIn subview drew a `COOKIE FALLBACK: OFF - OPTIONAL SSH PASTE OF cookies.txt`
  line under "SIGNED IN" on every signed-in visit — setup instructions no viewer acts
  on, crowding the status headline. The row is gone; the credential-ladder tier stays
  journal-only (`auth-mode=cookie|oauth` in `check_signin()` telemetry).
- The mpv OSC track-info row printed the external `--audio-file=` sidecar's mpv-derived
  title, which for YouTube's split googlevideo pair is the raw **URL query tail**
  (`webm&rqh=1&clen=...&sig=...`), wrapping the top-left corner into a wall of debug
  text after every seek and SponsorBlock cut (Ian report, live-probed on the Pi).
  `mpv-osc.lua` now drops URL-junk titles (`clean_title`), keeps real track names
  (Plex/Jellyfin commentary, `subinfo` friendly names) intact, and caps each info
  row to one line (`fit_oneline`). YouTube's row reads `AUDIO: ENG OPUS 2CH 48000 HZ`.
  Logic gate: 6/6 in a lua harness on the Pi against the patched file (live stream
  values mocked); nav-menu timing/UX untouched.

### Fixed — YouTube signed-in layer dying one hour after pairing (Playlists row vanished)
- YouTube access tokens live ~1 h. `check_signin()` reported only liveness, so an
  hour after pairing the account layer — the Playlists row, account feed and
  Channels — hid itself, and the refresh plumbing lived *behind* those gates:
  the app could never revive and silently fell back to file mode forever
  (2026-11-04 "where did the playlists view go?").
- A credential with a refresh token is now *usable*: expiry, slack, and revive
  semantics live in one predicate (`util/YtAuthGate.h`, unit-tested); the first
  auth touch refreshes before expiry, one swap at a time, and deferred account
  fetches (feed lists, playlist videos, FEchannels Channels) replay when the
  swap lands. A declined refresh falls back to file views with a journal
  nudge to re-pair on the CRT.

### Fixed — YouTube player stuck at "Loading…" forever
- The cold-path playback hold waited on `sponsorReady` with no expiry: when the SponsorBlock fetch fails transiently (journal: `sponsor transient err=5`) the backend gives up quietly and the signal never fires — the view stayed in "Loading…" forever, mpv never spawned, and Back/Escape went into dead IPC. Reproduced deterministically in the local lab (offscreen app + key channel + `/etc/hosts` bind poisoning only `sponsor.ajay.app` → same `err` → 20 s of silence, pre-fix).
- `Player.qml`: a 6 s `sponsorWaitTimer` restarted at the hold site now expires the hold and launches **without** segment EDL (late segments still apply via the property-filter path), and Back/Escape on a view whose playback never started returns natively via `goBack()` instead of shouting into a disconnected IPC socket.
- Verified with a local A/B race (identical cold path, poisoned domain): pre-fix = deadlock; post-fix = `wait-expired` at +6.0 s and mpv launches.

### Fixed — mpv playback overlay on the CRT
- The mpv 0.40.0 bump renamed `--stats` → `--load-stats-overlay` and **flipped its default to on**: every playback painted the stats/debug text (frame timings, A-V drift, cache state) on screen and spammed the journal with a line per second. The app now passes `--load-stats-overlay=no` explicitly (app-owned mpv v0.40.0 everywhere, so the flag is universal; lab harnesses that want the stats signal pass it themselves).

### Fixed — cold re-resolve per pick on live/rebroadcast rows
- Picking a wits.tv-style rebroadcast paid a fresh ~4 s yt-dlp resolve every time and cached nothing: the `<=480 avc1` format spec falls through to `best` for live sources, and yt-dlp hands back an HLS **manifest URL whose expiry lives in slash-delimited path segments** (`/expire/<epoch>/`) — the freshness parser only spoke query dialect (`?expire=`/`&expire=`), so the warm store silently refused (`stored=0`). Parser now accepts both dialects at the same strictness (3 new unit tests); first watch still resolves once, re-watches inside the expiry window open instantly.

### Changed — mpv OSC button rename (beta)
- On-screen playback control button relabeled `CROP` → `FILL` (both OSC scripts): with the geometry-driven auto-fill rule it toggles panscan fill on/off, and the old label lied about what it did. Internal `hide-crop` script opt and code comments aligned to the new name.

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

