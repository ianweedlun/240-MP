# Thanks & Attribution

Project policy (defined in `.hermes.md`): generative-AI artwork is for testing only;
production artwork must be human-made and libre (open source, fair use, CC), and
attribution is always required. Any libre code of others used in this project
deserves recognition and thanks. This file is that home for it.

## Typefaces

- **VCR OSD Mono** (`assets/fonts/VCR_OSD_MONO_1.001.ttf`), created by
  Riciery Santos Leal (a.k.a. mrmanet), distributed by its author as a free
  font via [dafont.com](https://www.dafont.com/vcr-osd-mono.font);
  redistributed here unmodified as the app's primary UI typeface.
  Terms recorded in `assets/fonts/LICENSE-vcr-osd-mono.txt`.
- **Unifont** (`assets/fonts/unifont.otf`), the GNU Unifont project
  (© Roman Czyborra, Paul Hardy, Qianqian Fang, Andrew Smith and others);
  dual-licensed SIL Open Font License 1.1 / GPL — full text and copyright
  list in `assets/fonts/LICENSE-unifont.txt`.

## Artwork derived from others' work

- **RetroArch module mark** (`modules/retroarch/assets/images/logo.svg`):
  geometry is the official libretro/RetroArch project icon
  (`libretro/RetroArch media/icons/icon.svg`, upstream commit-pinned),
  recomposed as one evenodd compound path in the app's single-color house
  style (filters/gradients stripped). The original icon is the
  libretro/RetroArch project's work, used here with attribution.

## Others' code we built on or referenced

- **The upstream 240-MP project** ([anthonycaccese/240-MP](https://github.com/anthonycaccese/240-MP)) —
  this fork's entire base application: modules, app shell, and mpv wiring.
- **mpv** (mpv.io) — the playback engine this frontend drives as a subprocess,
  including the IPC control channel, OSC scripting, and `--panscan` geometry.
- **Qt 6 / Qt Quick (QML)** — the UI framework and scene graph everything here
  is composed with (including the screenshot pipeline).
- **RetroArch source** ([libretro/RetroArch](https://github.com/libretro/RetroArch)) —
  consulted as the reference for `.LPL` playlist format handling and
  RZI-compressed playlist decompression (our parser follows the same
  LZO/RZI structure).
- **Open-Meteo** (open-meteo.com) — the weather/air-quality APIs the Weather
  module forecasts from (data source; no code copied).

## Pending — provenance not yet verified

- Weather module condition icons (`modules/weather/assets/images/wx/*.svg`):
  icon-name vocabulary matches Open-Meteo's documented weather icons; the
  drawing set's provenance has not been verified — recorded here as a
  placeholder until attribution (or a human-drawn replacement) settles the
  origin. See issue #33.
