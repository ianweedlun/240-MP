# PLEX-JWT-REFRESH-LATCH-01 regression suite

`plex-jwt-refresh-regression-tests` is a product-integrated Qt Test target that
compiles the real `PlexBackend.cpp` against a loopback `plex.tv` fixture. It
proves the auth-scoped watchdog releases `m_refreshInFlight` on a trickle stall,
calls back exactly once, and lets `refreshJwt()`, `handle498()` and
`checkAndRefreshOnStartup()` recover.

## Test seams (production code)

- `PlexBackend::setPlexTvUrlForTests()` repoints the file-scope `PLEX_TV` at the
  loopback fixture. No production caller.
- `PlexBackend::setAuthWatchdogMsForTests(int)` clamps to `[300, 60000]`, so a
  test cannot disable the watchdog. Product default stays 15000 ms.
- `PlexBackend` friends the test class and the `Rig` helper for guard/callback
  observation. No behavior is exposed to QML.

The shipped 5000 ms no-progress deadline (`kPlexTransferTimeoutMs` in
`plexRequest()`) is untouched; `controlGlobalTimeoutUnchanged` asserts it.

## Fixture

The trickle modes send a valid `200` status line with `Content-Length: 100000`
and then one body byte every 100 ms. Qt keeps the reply open and resets its
inactivity timer on every byte, so only a hard total deadline can end it. The
test watchdog is 300 ms and each stall is observed for 1200 ms (4x margin).

## Two-sided gate

GREEN (product source):

```sh
cmake -B build-green -DBUILD_TESTS=ON
cmake --build build-green --target plex-jwt-refresh-regression-tests -j4
QT_QPA_PLATFORM=offscreen ./build-green/plex-jwt-refresh-regression-tests
```

RED (pinned pre-fix source, same test code):

```sh
python3 src/tests/plex/make_red_snapshot.py <base-commit> /tmp/plex-red-src
cmake -B build-red -DBUILD_TESTS=ON -DPLEXJWT_SOURCE_ROOT=/tmp/plex-red-src \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0"
cmake --build build-red --target plex-jwt-refresh-regression-tests -j4
QT_QPA_PLATFORM=offscreen ./build-red/plex-jwt-refresh-regression-tests
```

The snapshot script copies the pre-fix `PlexBackend.{h,cpp}` from git and adds
only the test seams; it copies no watchdog logic. GREEN must pass all 21 cases;
RED must fail all 9 trickle/watchdog cases while the 12 controls pass on both.
Sanitized arms must link `libasan`/`libubsan` (verify with `nm -D`/`ldd`, not
CMakeCache) and report zero ASan/LSan/UBSan diagnostics.
