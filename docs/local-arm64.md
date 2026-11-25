# Local arm64 build lanes

Two ways to run CI's exact arm64 recipe locally — no CI minutes, no Pi
compilation. Both mirror the `ubuntu-24.04-arm` runner package-for-package
(see `.github/actions/build-and-test`) and both run ctest with the same
QML-layout exclude regex.

## One-time setup

binfmt_misc must be able to execute arm64 ELF (needed by every lane):

```
docker run --privileged --rm tonistiigi/binfmt --install arm64
```

Verify: `docker run --rm --platform linux/arm64 ubuntu:24.04 uname -m` → `aarch64`.

## Lanes

### `parity` — emulated arm64 container

```
./scripts/local-arm64.sh parity
```

Everything — compiler, linker, tests — runs inside an arm64 `ubuntu:24.04`
container under qemu-user. Byte-for-byte the CI runner environment, so a
result here is as close to CI green as it gets locally. Compilation is
emulated: a cold build takes roughly an hour; warm incremental builds with
ccache are minutes. Dependencies are baked into the committed
`240mp-build:arm64` image once (`images` mode), so no run pays an apt cost.
Use this lane when you want maximum confidence with zero configuration
divergence.

### `cross` — x86 cross-compiler + arm64 sysroot

```
./scripts/local-arm64.sh cross          # builds sysroot first if missing
./scripts/local-arm64.sh cross-sysroot  # sysroot only
```

Compiles at native x86 speed with `crossbuild-essential-arm64` against a
sysroot extracted (via `dpkg -x`, never installed) from the same arm64
packages CI installs. The toolchain file is `cmake/aarch64-toolchain.cmake`;
Qt host tools (moc, rcc) come from the container's amd64 `qt6-base-dev`
via `QT_HOST_PATH`. Test binaries are arm64 and run through binfmt with
`QEMU_LD_PREFIX` resolving libraries inside the sysroot. Cold builds are
several times faster than `parity`; the trade-off is that the environment
is assembled rather than native — a package that appears in the runner but
not in the sysroot closure will surface as a link or runtime error here,
which is still earlier and cheaper than CI.

### Shared behavior

- Build dirs: `~/.cache/240mp-arm64-builds/build-arm64` and `.../build-cross`
  — outside the repo on purpose, so a build dir is never shared with (or
  stomped by) another lab worker running cmake. `clean` removes them but
  keeps ccache, the sysroot and the baked images.
- ccache: `~/.cache/240mp-arm64-ccache` (shared; both lanes are arm64, so
  entries are compatible only between identical compilers — expect the first
  `cross` build after a `parity` build to be cold).
- Sysroot: `~/.cache/240mp-arm64-sysroot` (~1 GB). Rebuild with
  `cross-sysroot` after the CI apt package list changes.
- Both lanes emit a CI-layout tarball (`240-MP-local-*-arm64-<sha>.tar.gz`)
  so deployment-side scripts can be exercised without a CI run.
- Env knobs: `JOBS=<n>`, `NO_TESTS=1`, `BASE_IMAGE`/`PARITY_IMAGE`/`CROSS_IMAGE`
  (image refs; `images` mode re-bakes).

## What these lanes cannot do

No ALSA, DRM/KMS, evdev, or composite output — audio/display/device truth
still requires the Pi, and visual truth still requires the CRT. These lanes
replace CI and Pi *compilation* in the debug loop, not device validation.

## Known limits

- Qt 6.4 (noble) has no `QtQuick.Effects` or `QtQml.WorkerScript`, exactly as
  on the arm64 CI runner, so the QML-layout smokes are excluded here too.
  They run on the x86 lab build (Qt 6.11) as before.
- The `cross` lane is validated against the runner, not identical to it; when
  a `cross` result looks wrong, re-run the same commit through `parity`
  before suspecting the code.
- The `volume-stall-*` micro-benchmarks assert sub-16 ms heartbeat gaps and
  are excluded from local lanes only (emulation jitter breaks even the noop
  control by ~1 ms). CI still runs them.
- Out-of-tree builds set `MP240_TEST_REPO_ROOT` for tests that locate the
  checkout by walking up from the CWD (`ColorSchemesTest`); on CI the env is
  unset and the walk-up works unchanged.
