#!/usr/bin/env bash
# Local arm64 build lanes that mirror CI's native ubuntu-24.04-arm runner
# (see .github/actions/build-and-test). Debug locally; leave CI and the Pi
# out of the loop until a fix is green here.
#
# Usage:
#   ./scripts/local-arm64.sh parity         # arm64 container, everything runs under qemu
#   ./scripts/local-arm64.sh cross          # x86 cross-compiler + arm64 sysroot (native speed)
#   ./scripts/local-arm64.sh cross-sysroot  # (re)build the arm64 sysroot only
#   ./scripts/local-arm64.sh images         # (re)bake the two build images
#   ./scripts/local-arm64.sh clean          # remove local build dirs
#
# Options (set as env): JOBS=<n>  NO_TESTS=1
#
# One-time host setup — binfmt_misc must be able to exec arm64 ELF:
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# Both lanes configure with BUILD_TESTS=ON, run ctest with the arm64 lane's
# exclude regex, and pack the CI tarball layout. Parity matches the CI runner
# package-for-package; cross compiles at x86 speed against the same packages
# extracted into a sysroot. See docs/local-arm64.md for trade-offs.
#
# Containers run as the invoking host user (uid-mapped), so build outputs in
# the repo are never root-owned. Dependencies are baked into two committed
# images (240mp-build:arm64 / :amd64) — see the images mode — so no lane pays
# an apt cost per run and no lane ever needs root.
#
# Container bodies below are single-quoted bash -ec strings: an apostrophe
# inside them (even in a comment) breaks the outer quoting. The bash -n
# self-check before anything else makes that fail fast, not mid-build.
bash -n "$0" || exit 1
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/240mp-arm64-ccache}"
SYSROOT_DIR="${SYSROOT_DIR:-$HOME/.cache/240mp-arm64-sysroot}"
# Build dirs live outside the repo ON PURPOSE: the lab checkout is shared by
# parallel workers (cron/torture lanes), and a build dir inside it gets
# reconfigured or stomped by whoever runs cmake next.
BUILD_ROOT="${BUILD_ROOT:-$HOME/.cache/240mp-arm64-builds}"
BASE_IMAGE="${BASE_IMAGE:-ubuntu:24.04}"
PARITY_IMAGE="${PARITY_IMAGE:-240mp-build:arm64}"
CROSS_IMAGE="${CROSS_IMAGE:-240mp-build:amd64}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# Exact CI arm64 apt list (ci.yml build-linux-arm64).
# python3: required at configure time (CMakeLists find_package Python3) and preinstalled
# on the CI runner image even though the runner apt list does not name it.
CI_PKGS="build-essential cmake python3 qt6-base-dev qt6-declarative-dev libqt6svg6 qt6-svg-dev qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-window libdrm-dev libssl-dev libsdl2-dev libpcsclite-dev libqt6sql6 libqt6sql6-sqlite"
# amd64 host Qt dev packages supply the cross-building host tools (moc, rcc,
# qmlcachegen) that QT_HOST_PATH points at; they must match the sysroot's Qt
# 6.4 series.
CROSS_PKGS="build-essential cmake ccache openssl python3 pkg-config crossbuild-essential-arm64 qt6-base-dev qt6-declarative-dev"

# The exact exclude regex CI passes to the arm64 lane (ci.yml): those QML
# layout smokes need Qt >= 6.5 (QtQuick.Effects, QtQml.WorkerScript) and the
# noble runner ships 6.4. Keep in sync with ci.yml when it changes.
TEST_EXCLUDE="${TEST_EXCLUDE:-recent-view-smoke|plex-player-smoke|local-files-player-smoke|local-files-empty-path-smoke|weather-current-smoke|youtube-player-smoke|youtube-quit-intent-smoke|youtube-seek-smoke|youtube-telemetry-smoke|mpv-hold-repeat}"

# Local lanes add what qemu fundamentally cannot measure: wall-clock
# micro-benchmarks (volume-stall heartbeat gaps, the 16 ms mixer-constructor
# budget — emulation jitter breaks even the noop control) and the
# visitordata early-exit byte bounds (qemu TCP buffering delivers far more
# socket data before the client can abort). CI keeps enforcing all of them
# on the native arm64 runner.
LOCAL_TEST_EXCLUDE="${TEST_EXCLUDE}|volume-stall-alsa-delay200|volume-stall-alsa-delay0|volume-stall-alsa-edge|volume-stall-pipewire-delay200|volume-stall-pipewire-delay0|volume-stall-pipewire-edge|volume-stall-noop-control|pipewire-volume-mixer|yt-visitordata-cap-oversize-no-token|yt-visitordata-cap-oversize-token|yt-visitordata-cap-real-size-early-exit"

APP_VERSION="${APP_VERSION:-local-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"

mode="${1:-help}"
case "$mode" in
  parity) JOBS="${JOBS:-4}" ;;
  cross)  JOBS="${JOBS:-12}" ;;
esac

# Must exist before docker run: a missing mount source is created by the
# daemon as root and the uid-mapped container user cannot write to it.
mkdir -p "$BUILD_ROOT" "$CCACHE_DIR"

ensure_image() { # $1 = arch, $2 = tag, $3 = package list, $4 = optional root-time post-install
  if docker image inspect "$2" >/dev/null 2>&1; then return; fi
  local cname="240mp-build-bake-$$-$1"
  echo "baking image $2 (one-time; apt under $( [ "$1" = arm64 ] && echo qemu || echo native ))"
  docker rm -f "$cname" >/dev/null 2>&1 || true
  docker run --name "$cname" --platform "linux/$1" "$BASE_IMAGE" \
    bash -ec "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $3; ${4:-true}" \
    >/dev/null
  docker commit "$cname" "$2" >/dev/null
  docker rm "$cname" >/dev/null
  echo "image $2 ready"
}

run_parity() {
  ensure_image arm64 "$PARITY_IMAGE" "$CI_PKGS openssl ccache"
  # The whole toolchain is arm64, so compilation itself is emulated — slow
  # cold, ccache-fast warm. This is the byte-for-byte CI runner replica.
  docker run --rm --platform linux/arm64 --user "$HOST_UID:$HOST_GID" \
    -v "$REPO_ROOT":/src -v "$CCACHE_DIR":/ccache -v "$BUILD_ROOT":/build \
    -e CCACHE_DIR=/ccache -e HOME=/tmp \
    -e APP_VERSION="$APP_VERSION" -e TEST_EXCLUDE="$LOCAL_TEST_EXCLUDE" -e JOBS="$JOBS" \
    -w /src "$PARITY_IMAGE" bash -ec '
      export MP240_TEST_REPO_ROOT=/src
      cmake -B /build/build-arm64 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
        -DAPP_VERSION="$APP_VERSION" -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
      cmake --build /build/build-arm64 --parallel "$JOBS"
      [ "${NO_TESTS:-0}" = 1 ] || ctest --test-dir /build/build-arm64 --output-on-failure -j 4 -E "$TEST_EXCLUDE"
      cmake --install /build/build-arm64 --prefix /tmp/install/usr/local
      mkdir -p /tmp/install/scripts && cp scripts/240mp-ci-deploy.sh /tmp/install/scripts/
      tar -czf /src/240-MP-local-parity-arm64-${APP_VERSION#local-}.tar.gz -C /tmp/install .
    '
}

run_sysroot() {
  # The arm64 package closure is resolved and downloaded inside an arm64
  # container: apt sees a native system, so no cross-architecture resolution.
  # Extraction (dpkg -x) never executes package scripts, so this is emulation
  # only for apt itself — a few minutes, once per dependency change.
  docker run --rm --platform linux/arm64 \
    -v "$SYSROOT_DIR":/sysroot -e PKGS="$CI_PKGS" \
    "$BASE_IMAGE" bash -ec '
      set -e
      rm -rf /sysroot/*
      apt-get update -qq
      # Full recursive dependency closure. install --download-only is NOT
      # usable here: any arm64 package preinstalled in the base image (zlib,
      # lzma, ffi, pcre2...) is considered satisfied and never downloaded,
      # leaving holes that only surface as link-time undefined references.
      CLOSURE=$(apt-cache depends --recurse --no-recommends --no-suggests \
        --no-conflicts --no-breaks --no-replaces --no-enhances --no-pre-depends \
        $PKGS qml6-module-qtqml-workerscript | grep -v "^[[:space:]]" | grep -v "^<" | sort -u)
      mkdir -p /tmp/debs && cd /tmp/debs
      apt-get download $CLOSURE >/dev/null 2>&1 || true
      for deb in /tmp/debs/*.deb; do
        # Skip target-arch Qt tool binaries (qt6-base-dev-tools etc.) so the
        # tool lookup resolves to the host (amd64) packages via QT_HOST_PATH.
        case "$deb" in *-dev-tools_*) continue ;; esac
        dpkg -x "$deb" /sysroot
      done
      # dpkg -x does not run maintainer scripts, so the usrmerge symlinks that
      # packages normally create (/lib -> usr/lib etc.) never appear. arm64
      # ELF interpreters are registered at /lib/..., and qemu -L resolves them
      # through the sysroot — without these links target tools cannot run.
      # Some packages ship real files under /lib or /bin, so merge those into
      # /usr first and only then swap the directory for a symlink.
      for d in lib bin sbin; do
        if [ -d "/sysroot/$d" ] && [ ! -L "/sysroot/$d" ]; then
          mkdir -p "/sysroot/usr/$d"
          cp -a "/sysroot/$d/." "/sysroot/usr/$d/"
          rm -rf "/sysroot/$d"
        fi
        ln -sfn "usr/$d" "/sysroot/$d"
      done
      # The extracted glibc dev files are GNU ld scripts referencing
      # target-absolute paths (/lib/aarch64-linux-gnu/libm.so.6) that only
      # exist on the target — and would otherwise collide with the build
      # container amd64 multiarch dir. Rewrite them to point inside the
      # sysroot; the runtime .so files they name are all present there.
      for f in /sysroot/usr/lib/aarch64-linux-gnu/*.so; do
        if [ -f "$f" ] && grep -q "GNU ld script" "$f" 2>/dev/null; then
          # § is a throwaway placeholder so the /usr/lib rewrite does not
          # re-match inside the output of the /lib rewrite. The bare
          # /lib/ld-linux path comes third: it must be rewritten before the
          # /lib/aarch64 pass could ever see a /sysroot/lib result.
          sed -i "s|/usr/lib/aarch64-linux-gnu|§|g; s|/lib/ld-linux-aarch64.so.1|/sysroot/lib/ld-linux-aarch64.so.1|g; s|/lib/aarch64-linux-gnu|/sysroot/lib/aarch64-linux-gnu|g; s|§|/sysroot/usr/lib/aarch64-linux-gnu|g" "$f"
        fi
      done
      echo "sysroot: $(ls /sysroot/usr/lib/aarch64-linux-gnu/cmake | head -3 | tr "\n" " ")..."
    '
}

run_cross() {
  ensure_image amd64 "$CROSS_IMAGE" "$CROSS_PKGS" \
    "mkdir -p /usr/lib/aarch64-linux-gnu/qt6 && ln -sfn /sysroot/usr/lib/aarch64-linux-gnu/qt6/qml /usr/lib/aarch64-linux-gnu/qt6/qml"
  # Native-speed lane: x86 cross-compiler, arm64 sysroot, and the arm64 test
  # binaries executed through host binfmt (qemu-aarch64) with QEMU_LD_PREFIX
  # resolving their libraries inside the sysroot.
  if [ ! -d "$SYSROOT_DIR/usr/lib/aarch64-linux-gnu" ]; then
    echo "sysroot missing — building it first ($SYSROOT_DIR)"
    run_sysroot
  fi
  docker run --rm --platform linux/amd64 --user "$HOST_UID:$HOST_GID" \
    -v "$REPO_ROOT":/src -v "$SYSROOT_DIR":/sysroot:ro -v "$CCACHE_DIR":/ccache -v "$BUILD_ROOT":/build \
    -e CCACHE_DIR=/ccache -e HOME=/tmp -e ARM64_SYSROOT=/sysroot \
    -e APP_VERSION="$APP_VERSION" -e TEST_EXCLUDE="$LOCAL_TEST_EXCLUDE" -e JOBS="$JOBS" \
    -w /src "$CROSS_IMAGE" bash -ec '
      set -e
      export QEMU_LD_PREFIX=/sysroot
      export LD_LIBRARY_PATH=/sysroot/usr/lib/aarch64-linux-gnu
      export QT_QPA_PLATFORM=offscreen
      export QT_PLUGIN_PATH=/sysroot/usr/lib/aarch64-linux-gnu/qt6/plugins
      export PKG_CONFIG_SYSROOT_DIR=/sysroot
      export PKG_CONFIG_LIBDIR=/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:/sysroot/usr/share/pkgconfig:/sysroot/usr/lib/pkgconfig
      # QML engines resolve imports via the compiled-in Qt import path
      # (/usr/lib/aarch64-linux-gnu/qt6/qml), which only exists on the
      # target; the cross image bakes a symlink to the sysroot copy.
      export QML2_IMPORT_PATH=/sysroot/usr/lib/aarch64-linux-gnu/qt6/qml
      export MP240_TEST_REPO_ROOT=/src
      cmake -B /build/build-cross -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
        -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
        -DAPP_VERSION="$APP_VERSION" -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
      cmake --build /build/build-cross --parallel "$JOBS"
      [ "${NO_TESTS:-0}" = 1 ] || ctest --test-dir /build/build-cross --output-on-failure -j 4 -E "$TEST_EXCLUDE"
      cmake --install /build/build-cross --prefix /tmp/install/usr/local
      mkdir -p /tmp/install/scripts && cp scripts/240mp-ci-deploy.sh /tmp/install/scripts/
      tar -czf /src/240-MP-local-cross-arm64-${APP_VERSION#local-}.tar.gz -C /tmp/install .
    '
}

case "$mode" in
  parity)        run_parity ;;
  cross)         run_cross ;;
  cross-sysroot) run_sysroot ;;
  images)        ensure_image arm64 "$PARITY_IMAGE" "$CI_PKGS openssl ccache"
                 ensure_image amd64 "$CROSS_IMAGE" "$CROSS_PKGS" \
    "mkdir -p /usr/lib/aarch64-linux-gnu/qt6 && ln -sfn /sysroot/usr/lib/aarch64-linux-gnu/qt6/qml /usr/lib/aarch64-linux-gnu/qt6/qml" ;;
  clean)         rm -rf "$BUILD_ROOT/build-arm64" "$BUILD_ROOT/build-cross" "$REPO_ROOT/build-arm64" "$REPO_ROOT/build-cross"
                 echo "removed build dirs (ccache, sysroot and images kept:
  $CCACHE_DIR
  $SYSROOT_DIR
  $PARITY_IMAGE / $CROSS_IMAGE)" ;;
  *) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
