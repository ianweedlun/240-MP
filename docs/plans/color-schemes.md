# Plan: additional color-scheme presets (Dracula et al.)

**Branch:** `feature/color-schemes` (forked from `dev`) — docs only, no code yet.
**Tracks:** issue #15 on the fork.

## Current state
- Built-in presets live in `Main.qml` as a table of 5-key palettes
  (`"Video 1"`, `"Late Night"`, `"Synthwave"`, `"Terminal"`, `"T-120"`,
  `"Amber"`, `"Kinescope"`, `"SMPTE ECR 1-1978"`).
- `views/Settings.qml:44` builds the option list from those names and appends
  custom themes loaded by AppCore from `custom_color_schemes.json`
  (validated 5-key structure, strict name charset — `src/AppCore.cpp:423`).
- The 5-key palette contract + custom-scheme loading must not change; new
  presets are pure data additions.

## Proposed presets
Popular, well-documented terminal/editor palettes that fit the 5-key contract
(background, foreground/ink, accent/selection, dim/secondary, highlight):
- **Dracula** — github.com/dracula/dracula-theme (anchor for this request); bg #282a36, ink #f8f8ff, accent #bd93f9/#ff5555.
- **Tokyo Night** (VS Code extension palette) — bg #1a1b26, accent #7e8ffa/#ff7e5f.
- **Nord** — bg #2e3440, accent #88c0d0.
- **Solarized Dark** — precisely-specified hues (#002b36 bg, #b5d0c7 ink, accent #268bd2).
- **Monokai** — bg #272727, accent #f59120/#f73f5f.
- (stretch) **Obsidian**, **Green Phosphor** classic CRT green to match the CRT target hardware.

Selection rules: prefer palettes with published hex sets (no eyeballed guesses);
name keys exactly like existing themes; each theme entry cites its upstream
source in a comment (license check per palette — Solarized is well-documented
public, Dracula repos are per-repo, colors themselves are unencumbered).

## Implementation sketch (when this gets built)
1. `Main.qml` theme table — add entries in the same shape as `"Synthwave": {…}`.
2. No Settings.qml change needed — option list is derived from the table.
3. Ordering: keep built-ins first (existing order is deliberate retro→modern);
   insert new presets after "Kinescope"/"SMPTE" so existing indices/values don't
   shift.
4. Cite sources in comments; validate on Pi with a Settings screenshot per
   project rules (row visible + value applies on relaunch).

## Open questions for the user
- Which subset? (proposing Dracula, Tokyo Night, Nord, Solarized Dark, Monokai;
  trimming if any clash on the CRT.)
