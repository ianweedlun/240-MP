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
  The SponsorBlock integration speaks mpv's EDL virtual-timeline format
  (mpv-player/mpv `DOCS/edl-mpv.rst`, GPL-3.0) — grammar followed, no code
  copied; skip-by-omission and titled boundary cuts are its mechanisms.
- **Qt 6 / Qt Quick (QML)** — the UI framework and scene graph everything here
  is composed with (including the screenshot pipeline).
- **RetroArch source** ([libretro/RetroArch](https://github.com/libretro/RetroArch)) —
  consulted as the reference for `.LPL` playlist format handling and
  RZI-compressed playlist decompression (our parser follows the same
  LZO/RZI structure).
- **Open-Meteo** (open-meteo.com) — the weather/air-quality APIs the Weather
  module forecasts from (data source; no code copied).
- **red** ([keshavbhatt/red](https://github.com/keshavbhatt/red), GPL-3.0-or-later)
  — its SponsorBlock client design (k-anonymous 4-hex SHA-256 prefix lookup,
  exact-videoID collision filtering, per-category Off/Mark/Skip) is the basis
  for our port-adjacent `src/util/YtSponsorEdl.h` + backend fetch layer:
  semantics ported, code written fresh, names/namespaces ours.
- **FreeTube** (FreeTubeApp/FreeTube) and **innertube** (tombulled/innertube)
  — prior-art reference for InnerTube browse/FEchannels list shapes and
  pagination (design consulted; no code copied).
- **yt-dlp** (yt-dlp/yt-dlp, GPL-3.0) — consulted for extractor client/`player_skip`
  knowledge; still invoked as our insurance for pathological streams only.

## Color palettes we borrowed (published data, human-designed)

- **Dracula** — the Dracula theme palette (Jonathan Bartlett), published openly at
  [draculatheme.com/contribute](https://draculatheme.com/contribute).
- **Tokyo Night** — published palette of the tokyo-night VS Code theme
  ([github.com/tokyo-night/tokyo-night-vscode-theme](https://github.com/tokyo-night/tokyo-night-vscode-theme)).
- **Nord** — published "Nord" terminal color dumps (vim-Nord / Gogh colors.properties
  catalogs).
- **Solarized Dark** — the Solarized palette designed by Julian Iveson, spec by Wes
  Cherry, published openly (base-N hex values across terminal-theme catalogs).
- **Monokai** — the Monokai scheme (Andy Wightman, Sublime Text), as published at
  [monokai.pro](https://monokai.pro/) and the base46 theme catalogs.

These are color data adopted as built-in themes in `Main.qml`; each entry cites its
source inline. No generative-AI colors: every hex set is a human-published spec.

## Virtual Channels preset logos (Wikimedia Commons, Public Domain)

Monochrome corner marks under `modules/virtual_channels/assets/logos/*.png`, rendered from Public Domain SVG logos on Wikimedia Commons:

- `20th-century-fox.png`: [File:20th Century Studios.svg](https://commons.wikimedia.org/wiki/File:20th_Century_Studios.svg) (Public domain)
- `a24.png`: [File:A24 logo.svg](https://commons.wikimedia.org/wiki/File:A24_logo.svg) (Public domain)
- `abc.png`: [File:ABC-2021-LOGO.svg](https://commons.wikimedia.org/wiki/File:ABC-2021-LOGO.svg) (Public domain)
- `adult-swim.png`: [File:Adult Swim 2003 logo.svg](https://commons.wikimedia.org/wiki/File:Adult_Swim_2003_logo.svg) (Public domain)
- `amc.png`: [File:AMC logo 2016.svg](https://commons.wikimedia.org/wiki/File:AMC_logo_2016.svg) (Public domain)
- `bbc.png`: [File:BBC.svg](https://commons.wikimedia.org/wiki/File:BBC_logo_(1997-2021).svg) (Public domain)
- `cartoon-network.png`: [File:Cartoon Network 1992 logo.svg](https://commons.wikimedia.org/wiki/File:Cartoon_Network_logo_(1992-2004).svg) (Public domain)
- `cbs.png`: [File:CBS logo.svg](https://commons.wikimedia.org/wiki/File:CBS_logo.svg) (Public domain)
- `channel-4.png`: [File:Channel 4 2022.svg](https://commons.wikimedia.org/wiki/File:Channel_4_2022.svg) (Public domain)
- `cnn.png`: [File:CNN.svg](https://commons.wikimedia.org/wiki/File:CNN.svg) (Public domain)
- `columbia.png`: [File:Sony Pictures Television.svg](https://commons.wikimedia.org/wiki/File:Sony_Pictures_Television.svg) (Public domain)
- `comedy-central.png`: [File:Comedy Central 2018.svg](https://commons.wikimedia.org/wiki/File:Comedy_Central_2018.svg) (Public domain)
- `discovery.png`: [File:Discovery Channel - Logo 2019.svg](https://commons.wikimedia.org/wiki/File:Discovery_Channel_-_Logo_2019.svg) (Public domain)
- `disney.png`: [File:Disney wordmark.svg](https://commons.wikimedia.org/wiki/File:Disney_wordmark.svg) (Public domain)
- `dreamworks.png`: [File:DreamWorks-Logo.svg](https://commons.wikimedia.org/wiki/File:DreamWorks-Logo.svg) (Public domain)
- `espn.png`: [File:ESPN wordmark.svg](https://commons.wikimedia.org/wiki/File:ESPN_wordmark.svg) (Public domain)
- `fox.png`: [File:Fox Broadcasting Company logo (2019).svg](https://commons.wikimedia.org/wiki/File:Fox_Broadcasting_Company_logo_(2019).svg) (Public domain)
- `fx.png`: [File:FX 2008 logo.svg](https://commons.wikimedia.org/wiki/File:FX_2008_logo.svg) (Public domain)
- `hanna-barbera.png`: [File:Hanna-Barbera Logo.svg](https://commons.wikimedia.org/wiki/File:Hanna-Barbera_Logo.svg) (Public domain)
- `hbo.png`: [File:HBO logo.svg](https://commons.wikimedia.org/wiki/File:HBO_logo.svg) (Public domain)
- `lionsgate.png`: [File:Lionsgate Canada logo.svg](https://commons.wikimedia.org/wiki/File:Lionsgate_Canada_logo.svg) (Public domain)
- `lucasfilm.png`: [File:Lucasfilm logo.svg](https://commons.wikimedia.org/wiki/File:Lucasfilm_logo.svg) (Public domain)
- `marvel.png`: [File:Marvel Logo.svg](https://commons.wikimedia.org/wiki/File:Marvel_Logo.svg) (Public domain)
- `mgm.png`: [File:Metro-Goldwyn-Mayer logo.svg](https://commons.wikimedia.org/wiki/File:Metro-Goldwyn-Mayer_logo.svg) (Public domain)
- `miramax.png`: [File:Miramax logo.svg](https://commons.wikimedia.org/wiki/File:Miramax_logo.svg) (Public domain)
- `nbc.png`: [File:NBC logo 2022.svg](https://commons.wikimedia.org/wiki/File:NBC_logo_2022.svg) (Public domain)
- `new-line-cinema.png`: [File:New Line Cinema.svg](https://commons.wikimedia.org/wiki/File:New_Line_Cinema.svg) (Public domain)
- `nickelodeon.png`: [File:Nickelodeon 2009 logo.svg](https://commons.wikimedia.org/wiki/File:Nickelodeon_2009_logo.svg) (Public domain)
- `paramount.png`: [File:Paramount Pictures Corporation logo.svg](https://commons.wikimedia.org/wiki/File:Paramount_Pictures_Corporation_logo.svg) (Public domain)
- `pbs.png`: [File:PBS logo 2019.svg](https://commons.wikimedia.org/wiki/File:PBS_logo_2019.svg) (Public domain)
- `pixar.png`: [File:Pixar logo.svg](https://commons.wikimedia.org/wiki/File:Pixar_logo.svg) (Public domain)
- `showtime.png`: [File:Showtime.svg](https://commons.wikimedia.org/wiki/File:Showtime.svg) (Public domain)
- `studio-ghibli.png`: [File:Studio Ghibli wordmark.svg](https://commons.wikimedia.org/wiki/File:Studio_Ghibli_wordmark.svg) (Public domain)
- `syfy.png`: [File:SYFY.svg](https://commons.wikimedia.org/wiki/File:SYFY.svg) (Public domain)
- `the-cw.png`: [File:The CW.svg](https://commons.wikimedia.org/wiki/File:The_CW_(2006%E2%80%932024).svg) (Public domain)
- `toho.png`: [File:Toho logo.svg](https://commons.wikimedia.org/wiki/File:Toho_logo.svg) (Public domain)
- `universal.png`: [File:Universal Pictures logo.svg](https://commons.wikimedia.org/wiki/File:Universal_Pictures_logo.svg) (Public domain)
- `warner-bros.png`: [File:Warner Bros. Pictures 2023 (Alt).svg](https://commons.wikimedia.org/wiki/File:Warner_Bros._Pictures_2023_(Alt).svg) (Public domain)

## Pending — provenance not yet verified

- Weather module condition icons (`modules/weather/assets/images/wx/*.svg`):
  icon-name vocabulary matches Open-Meteo's documented weather icons; the
  drawing set's provenance has not been verified — recorded here as a
  placeholder until attribution (or a human-drawn replacement) settles the
  origin. See issue #33.
