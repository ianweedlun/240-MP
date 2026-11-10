# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

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

