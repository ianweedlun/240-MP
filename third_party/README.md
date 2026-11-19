# Vendored third-party dependencies

Everything under this directory is vendored so the project builds with **no network fetch**.
Each entry records the upstream, the exact commit, the licence, and every local patch.

## innertube-qt

- Upstream: https://github.com/BowDown097/innertube-qt
- Vendored at: `7252590b5a66d56232769ad8d7181117e85aa1b5` (`master`, pushed 2026-05-24)
- Licence: GPL-3.0 (matches this project)
- Purpose: in-process YouTube Innertube client. Replaces the per-play `yt-dlp -g` resolve on
  the playback critical path; `yt-dlp` stays as a fallback for one release.

### Local patches to `third_party/innertube-qt/CMakeLists.txt`

1. **`Widgets` added** to `find_package(Qt6 ...)` and to `target_link_libraries`.
   `responsiveimage.cpp` includes `<QApplication>`, which lives in Widgets.
2. **Built with `-DINNERTUBE_NO_WEBENGINE=ON`.** The auth store's WebEngine includes are guarded
   by that macro, and the app never renders a web page itself.
3. **`expected-lite` resolved from the vendored copy** rather than `FetchContent`, so a CI runner
   with no GitHub reach still builds.
4. **`probe/` is not vendored** — that directory was spike-only scaffolding.

### Local source patches

1. **`StreamingFormat` exposes `audioTrack.displayName`, `audioTrack.id`, and
   `audioTrack.audioIsDefault`.** The resolver needs these fields to choose the
   video's default-language audio instead of the first listed dub.

## verdigris

- Upstream: https://github.com/woboq/verdigris
- Vendored at: `8abfb8d69c689e2c607225c98baa829f839686f9`
- innertube-qt's submodule pin: `9b3e3f26251444bf48011202db85c892b0f45c57` — the submodule ships
  **empty** in the release tarball, so the sources are vendored directly.
- Licence: LGPLv3
- Required because `innertubeexception.h` includes `<wobjectdefs.h>`; verdigris provides the
  moc-free `wobjectdefs.h`/`wobjectcpp.h` pair.

## expected-lite

- Upstream: https://github.com/martinmoene/expected-lite
- Vendored at: tag `v0.8.0` (commit `182165b`)
- Licence: **Boost Software License 1.0** (see `expected-lite/LICENSE.txt`)
- Required because innertube-qt uses `nonstd::expected` throughout.

## Updating

Re-vendor by checking out the upstream commit, copying the tree, and re-applying the
innertube-qt patches above. Verify with a clean build:

```bash
cmake -B build-test -DBUILD_TESTS=ON && cmake --build build-test -j"$(nproc)" && ctest --test-dir build-test
```
