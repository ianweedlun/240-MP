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

