# mpv.conf as the source of truth (no "Mpv Conf" pseudo-option)

Branch: `feature/mpv-conf-direct-edit` (from dev). Replaces the override-layer
model in the merged `feature/mpv-config-options` work.

## Design
- The Settings rows ARE the user's `~/.config/mpv/mpv.conf`: reading it IS the
  state; picking an option writes it (replace-in-place or append).
- No `appSettings` storage for these four keys. One-time migration: legacy
  `config.json` app-section values get written into mpv.conf at startup and
  dropped (their "Mpv Conf" pseudo-state → nothing written, file wins).
- Display = file value mapped back to menu tokens; values outside the menu's
  whitelist show honestly as `Custom (<raw>)` (user chose this over a
  "keep file value" pseudo-row); `include <file>` followed ONE level so
  split configs display correctly.

## Parser semantics (src/util/MpvBasicOptions.*)
- table-driven (one ChoiceRow/ NumericRow per managed option; adding an mpv
  setting = adding a row)
- `#` comments ignored; LAST active assignment wins (matches mpv); underscore
  spelling tolerated
- byte-preserving write-back: only the touched line changes

## Scope decisions (agreed this session)
- sound_boost off-menu dB values: show `Custom (-7.5 dB)`, change snaps to
  nearest menu step (no slider on the 240px panel)
- includes: one level deep; deeper nests are mpv's job
- spawn args unchanged: mpv reads its own conf; we append nothing basic
