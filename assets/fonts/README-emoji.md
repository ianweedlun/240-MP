# Emoji font subset

`Symbola-emoji-subset.ttf` is a subset of Symbola 2.60 (Debian
`fonts-symbola`), reduced to Unicode ranges `2600-27BF`, `1F000-1FAFF`, and
`1FC00-1FFFD`. It supplies monochrome outline glyphs for emoji in
server-supplied titles (YouTube, Plex, Jellyfin, Emby, weather) while matching
the CRT aesthetic and keeping the install delta under 2 MB.

Generated with fontTools:

```
/usr/bin/python3 -m fontTools.subset Symbola_hint.ttf \
  --unicodes='2600-27BF,1F000-1FAFF,1FC00-1FFFD' \
  --layout-features='' --no-hinting --drop-tables+=DSIG \
  --output-file=Symbola-emoji-subset.ttf
```

Upstream: https://dn-works.com/ufas/
Copyright 2007-2015 George Douros. License: Douros-Free-Font (free for any
use, including modification and redistribution) — full text in
`LICENSE-Symbola.txt`.

After subsetting, the `name` table's family IDs (1, 4, 6, 16) are rewritten to
`Noto Color Emoji`. That alias is deliberate: Qt resolves that family in its
automatic glyph fallback, so `Text` elements using the app font family render
emoji without a comma-separated `font.family` list. The font itself is
monochrome; the name is only a compatibility key for Qt's fallback lookup.
`Main.qml` registers it through a deferred FontLoader and leaves `globalFont`
unchanged. The same file is visible to mpv/libass because `MpvController`
exports `FONTCONFIG_FILE` pointing at this directory. Outline glyphs matter
here: libass cannot use CBDT/CBLC color-bitmap fonts as a fallback, so a color
emoji font fixes Qt text but leaves mpv OSD tofu.

Coverage gap: Symbola 2.60 has no glyphs above U+1F9E6, so newer emoji in
U+1FA70-U+1FAFF (for example U+1FAF8) remain uncovered. Symbols in
U+2600-U+27BF and U+1F300-U+1F6D4 — including the trophy, party popper,
smiley and heart used in the probe — are covered.
