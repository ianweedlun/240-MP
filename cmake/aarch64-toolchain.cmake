# Cross-compilation toolchain for local arm64 builds — used only by
# scripts/local-arm64.sh cross, which runs the x86 cross toolchain against a
# sysroot extracted from the same ubuntu:24.04 arm64 packages CI's native
# runner installs (.github/actions/build-and-test).
#
# Deliberately NOT set here: CMAKE_SYSROOT. Target glibc comes from the cross
# toolchain's own /usr/aarch64-linux-gnu tree; pointing --sysroot at the
# package sysroot would give the compiler a second, shadowing glibc. The
# sysroot supplies Qt and project dependencies through CMAKE_FIND_ROOT_PATH.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_ARM64_CROSS_PREFIX "$ENV{ARM64_CROSS_PREFIX}")
if(NOT _ARM64_CROSS_PREFIX)
  set(_ARM64_CROSS_PREFIX "/usr/bin/aarch64-linux-gnu-")
endif()
set(CMAKE_C_COMPILER "${_ARM64_CROSS_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_ARM64_CROSS_PREFIX}g++")

set(_ARM64_SYSROOT "$ENV{ARM64_SYSROOT}")
if(NOT _ARM64_SYSROOT)
  set(_ARM64_SYSROOT "/sysroot")
endif()

set(CMAKE_FIND_ROOT_PATH "${_ARM64_SYSROOT}" "${_ARM64_SYSROOT}/usr" "/usr/aarch64-linux-gnu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# BOTH, not ONLY: Qt6's config finds its host tools (moc/rcc) through
# find_package(Qt6HostInfo PATHS "${QT_HOST_PATH}") — with ONLY that PATHS
# hint is re-rooted into the sysroot, the host Qt is invisible and Qt falls
# back to arm64 tools that must run under qemu. Root order keeps the target
# Qt6 config (sysroot) ahead of any host package.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# The linker only searches its own default dirs for the DT_NEEDED chain of
# the shared libraries being linked — those all live in the sysroot's
# multiarch dir, so hand it over explicitly. libproxy keeps its backend
# module in a subdirectory of its own; cover that too. Joined as ONE string:
# a CMake list here expands with ';' into the link command line and only the
# first flag survives.
set(_ARM64_LINK_FLAGS
    "-Wl,-rpath-link,${_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,${_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu/libproxy -Wl,-rpath-link,${_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu/pulseaudio -L${_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu")
foreach(_linker_type EXE SHARED MODULE)
  set(CMAKE_${_linker_type}_LINKER_FLAGS_INIT "${_ARM64_LINK_FLAGS}")
endforeach()

# Debian puts arch-specific headers (opensslconf.h, SDL's _real_SDL_config.h)
# under /usr/include/<triplet>/; the cross gcc's built-in triplet dir is the
# toolchain's own, not the sysroot's, so add the sysroot one explicitly.
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${_ARM64_SYSROOT}/usr/include/aarch64-linux-gnu")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${_ARM64_SYSROOT}/usr/include/aarch64-linux-gnu")

# Qt6's CMake config requires a host Qt of the same version to supply moc/rcc
# and other native tools when cross-compiling. The cross container installs
# amd64 qt6-base-dev (same noble 6.4 series) for this. QT_HOST_PATH_CMAKE_DIR
# is what Qt6's tool-dependency helper actually prepends to the tool search
# (QtPublicDependencyHelpers) — QT_HOST_PATH alone is not enough on distro
# Qt, where the tool packages live under the multiarch cmake dir.
set(QT_HOST_PATH "/usr")
set(QT_HOST_PATH_CMAKE_DIR "/usr/lib/x86_64-linux-gnu/cmake")
