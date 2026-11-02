# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Added — Built-in color scheme palettes (#15, feature/color-schemes)
- Five new built-in schemes in the themes map (`Main.qml`) + matching Settings picker
  entries (`views/Settings.qml`): Dracula, Tokyo Night, Nord, Solarized Dark, Monokai.
  All hex sets taken from published specs only (draculatheme.com palette, tokyo-night
  VS Code theme README, Gogh-based "Nord" colors.properties dump, Solarized spec,
  base46 Monokai catalog); role mapping (primary/secondary/tertiary/surface/accent)
  documented inline per entry. Sources credited in-code; `THANKS.md` acknowledges the
  palettes as human-made libre works.
- Unit test `palette-tests` (`src/util/tests/ColorSchemesTest.cpp`): parses the themes
  table and the picker list from source, asserts exact agreement, five well-formed hex
  keys per built-in, and the five new names present. Pure text-in, no QML engine.

### Added — Deploy tree-skip + repo-hygiene guardrails (dev, 2026-11-02)
- `deploy-pi.yml` + `scripts/deploy-pi.sh`: tree-skip fast path. A verified deploy writes the
  pushed tree hash into `~/.240mp-last-deployed-tree` on the Pi; the next same-tree run skips
  upload + root-side dance (no Pi reboot for byte-identical trees). Tree-level gate, not binary
  md5 — tarball also carries QML assets.
- `.gitignore` guardrail: `build-*/`, `*.local.bak/` (a retroarch tail once committed ~400 build-artifact
  files; now untrackable).
- Evidence screenshots (`evidence/**`, 17 files) untracked from the tree — validation artifacts live
  on the tracking issues; older ones remain recoverable via git history at the commits that validated them.

### Fixed — Skip-marker ownership (dev)
- Marker verified live at `/home/ghdeploy/.240mp-last-deployed-rev` (PI_SSH_USERNAME=ghdeploy;
  earlier probes looked in the wrong home — 700 default umask made it invisible to ian).
  `deploy-pi.sh` now reads the authoritative CI marker first; its own home keeps a private copy.
  One Pi-side permission: `/home/ghdeploy` o+rx so the local path can traverse.

### Fixed — Deploy skip-gate keying (dev)
- The tree-skip fast path broke on its first live run: the workflow's GITHUB_TOKEN is
  Actions/Metadata-scoped, so the `/repos/.../commits` tree lookup 404'd (and a `${GITHUB_TOKEN}`
  reference was empty), failing the deploy step outright. Re-keyed to the commit-level gate
  (`~/.240mp-last-deployed-rev`) sourced event-natively from `github.event.workflow_run.head_sha`
  — no API call; never-wrong skip preserved, tree-level rationale kept in the workflow comments.

### Changed — Telemetry portability contract line (dev)
- Contract gains a Code Conventions section: telemetry primitives (CMake `TELEMETRY` option,
  `telemetryEnabled` signals, `AppCore::log/startTelemetrySession`) are pinned app-level, outside
  every module; modules only consume them and new modules inherit via the module pattern. Cashes
  #38's portability intent into the contract so future bursts never re-derive it.

### Changed — Dev-first branch model (dev)
- Dev-first branch model adopted as the integration contract. All area branch tips (retroarch,
  weather, mpv-config, plex-quality, color-schemes) were merged into `dev` (single merge commits,
  docs resolved to dev, unit tests carried along); Pi repo reset to dev and redeployed via the
  CI→deploy pipeline with md5 gate `869bf326…` → `b533d4e8…`, service active.
- Contract text hygiene (same commit set): `.hermes.md` keeps present-tense facts only — removed
  restatements (main-mirror rule duplicated in DO-NOTS; dev-fork wording twice), retired-machinery
  references (`docs/plans/*`, `issue-watch.sh`, dev-baseline roster, GitHub/Build/Trust echo
  sections), and inert history (pre-migration commit identities) relocated to
  `240-mp-development references/comms-attribution.md` (§ Repo history archaeology) so it stays
  pullable; `.hermes.megacave.md` re-synthesized in lockstep (drift gate green).
- History graft (later same day): `d77725c` = commit-tree graft of `upstream/main` into dev
  (tree unchanged, zero content) — dev/main/upstream had unrelated roots since project setup re-rooted
  dev; the "N commits behind upstream" badge was a topology artifact, now 0-behind. Never rebase dev.

### Fixed — Auto Fit silent no-op on the Pi (dev)
- Auto Fit never fired on any Pi playback (letterboxes identical with the setting
  ON or OFF). Three stacked silent failures in one IPC path:
  (1) fit-target reads used `width`/`height` — mpv reports the *source decode*
  size there, so computed window aspect always equaled the content aspect and the
  wider-than-display verdict never triggered; correct properties are
  `display-width`/`display-height`.
  (2) the `get_async <id> <prop>` form is not mpv syntax — mpv answered
  "invalid parameter" and the reply was discarded; `request_id` is a top-level
  *integer* JSON field.
  (3) success replies carry `error: "success"`, which the parser read as a
  failure.
- All three fixed: `get_property display-width/height` with top-level integer
  request_id, `error:"success"` accepted, and the fit decision is now visible:
  journal lines `[MpvController] auto-fit: display size known …` per mpv launch
  and `auto-fit verdict: … panscan … (sweep on|)` per file load. If the VO can't
  report display size, a `display-size read failed` warning explains why fit stays
  off. Pure fit math (mpvfit/AutoFit.h) unchanged, still unit-tested.
- Follow-up (same day): DRM VOs have no windowing backend at all — `display-width`
  read answered `property unavailable` on the Pi. The fit target now comes from the
  app's own startup display enumeration (main.cpp hands its QScreen geometry to
  `AppCore::setDisplayGeometry`; on EGLFS that IS the KMS plane `vo=drm` draws to).
  The mpv IPC read remains only as the desktop fallback. Journal now logs
  `auto-fit: display size from app (…px)` at mpv connect.
- Second follow-up (same day): `video-params/aspect` is NOT in mpv's observable
  property-change set — the observer never fired (verified by journal silence on
  Pi 0.40). Content aspect now read via a single blocking `get_property` sent at
  IPC connect — get_property waits until the property becomes available, so the
  reply lands at first file load; echoed on integer request_id 9003. (The
  intermediate `file-loaded`-event attempt was caught pre-deploy: that name is a
  Lua hook point, not an IPC event.) Journal gains `auto-fit verdict: …` per
  launch.

### Added — Debug key-channel capture token (dev)
- The telemetry-gated debug key channel (`/tmp/240mp_key.in`, same `deliverPress`
  chokepoint as real input) now accepts `shot`/`f12` lines: invokes the same
  `AppCore::takeScreenshot` capture as the physical/injected capture keys. Local
  offscreen runs (arm64 binary under WSL) get scripted screenshots in `/tmp`,
  view-tagged filenames included; Pi behavior unchanged.

### Added — Plex resolution-aware direct play (#14, feature/plex-res-direct-play)
- Direct-play/transcode decision from stream-metadata resolution class vs the quality ladder: at/below threshold direct, above transcodes; shorter edge for portrait/rotated files; unknown dimensions transcode (safe CRT branch).
- Quality options relabeled to the new semantics: "Direct Play (always)", "Above 1080p/720p/480p: transcode" (corrected ladder ids 8000/3000/1500; legacy custom ids keep always-transcode).
- Header-only decision logic (`PlaybackDecision.h`) with boundary-case unit tests (ctest-registered); playback decision telemetry line in state log.

### Added — Weather current-conditions AQ/UV rows (#29, #39, feature/weather-osd-signals)
- `rowTexts()` bakes must-show priority directly: AIR QUALITY and UV INDEX lead the
  column before the budget fit; the old prefix-derived order cut both rows at Pi
  display height. Dead `rowOrder`/`rowPriority` indirection removed.
- #39's dangling-reply suspect disproved (Qt `deleteLater()` is deferred); real gap
  was the QML order above. Evidence screenshots on branch (`evidence/weather-osd-aq-uv/`).

### Fixed — Weather location dial (#37, feature/weather-osd-signals)
- LocationPicker highlight now anchors to the glyph Text inside its delegate
  (ChoiceOverlay pattern); the v1 delegate-arithmetic box misrendered offset
  across cells, disproved on the screenshot wall. Grid placement stays
  explicit — bare-Repeater placement misrendered too (v2, also disproved).
- Screenshot capture trigger accepts `F12 ∪ Tab` in the view: Qt's EGLFS key
  map drops F-series codes in this build (probed evdev 87–94), while `Tab`
  delivers as `Qt.Key_Tab` end-to-end; physical F12 unaffected.
- Verified end-to-end on the Pi: state-driven nav, dial cell move
  (0→Down+Right→`5`), capture `240mp-LocationPicker_v6_20261102_020145.png`;
  CI + macOS + Pi-probe green for `edcbdbf8`; deploy md5 `467c7d02…`.

### Added — mpv playback settings (feature/mpv-conf-direct-edit, #30)
- Settings → Settings tab → Playback submenu: live-editable playback controls (`auto_fit`,
  video pan-scan levels, audio normalize/boost, `deinterlace`, `audio_channels`), read from and
  persisted via `appCore.get_settings()`/`save_setting` and applied to mpv.conf at playback
  (`src/player/MpvController`).
- Auto-fit fills the display for any content; oversized content pans & scans across the CRT
  (sweep timer connected via `QTimer::timeout` — the Qt6 signal house idiom).
- Settings tab made reachable: Tab capture key ungated (injected key events are raw,
  not app-synthetic; the 37c0d1e synthetic-event gate never fired).
- PlaybackSettings.qml rewritten onto the proven Settings/Repeater subview contract: the
  previous file was pseudo-QML the engine never parsed (blank blue submenu on the CRT).
  Validated with the Pi's `qmldom` (new file PARSE-OK; old file confirmed invalid by the
  same validator).

### Added — Housekeeping & policy (dev, #31/#33)
- Issue-post attribution now carries the git commit identity as a trailer-style footer
  line (`Co-authored-by: Hermes Agent <hermes-agent@240mp.local> — automated agent post`);
  the marker phrase keeps the issue-feed watcher's detection contract unchanged (#31).
- Artwork & attribution policy defined in `.hermes.md` (AI art = testing only; production
  art human-made + libre, attribution mandatory) and `THANKS.md` created with verified
  provenance for the bundled fonts, the RetroArch-derived module mark, and base
  dependencies (#33). Weather condition-icon provenance flagged pending.

### Added — Agent ingest mirror (megacave, docs pipeline)
- `.hermes.megacave.md`: single synthesized compressed mirror of the whole `.hermes.md`
  contract (Classical-Chinese register; identifiers/paths/commands verbatim). Header carries
  `SRC-BLOB:` = the git blob of the `.hermes.md` it was synthesized from; cheap session-start
  freshness check is a blob compare (`git ls-tree origin/dev .hermes.md`), match → ingest the
  mirror, drift → Ian-readable rules and the mirror is re-synthesized. Ian-readable remains
  source of truth; the mirror is never hand-edited.
- `docs-lint.sh` gained a drift gate: SRC-BLOB vs live dev blob; drift opens a `DOCS DRIFT`
  issue on the fork (agent account via curl+Bearer, deduped against open drift issues).
- `sync-hermes.yml` and `docs-mirror.sh` carry both doc blobs to every branch (uniformity
  set extended to four files); `CLAUDE.md` pointer updated to check drift before choosing
  which side to read.

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

