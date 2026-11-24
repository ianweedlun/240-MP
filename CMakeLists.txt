cmake_minimum_required(VERSION 3.21)
project(240mp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Strip qDebug(...) calls in Release builds to reduce CPU/disk noise on Pi
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DQT_NO_DEBUG_OUTPUT")

# Debug telemetry (screen-state log, row-selection and input-decision signals
# for remote test loops). Production builds: -DTELEMETRY=OFF compiles the C++
# bodies out; the QML emit/listener wiring then calls an inert stub.
option(TELEMETRY "Debug telemetry for remote test loops" ON)
if(TELEMETRY)
    add_compile_definitions(APP_TELEMETRY)
endif()

# Release version string. CI passes the git tag (-DAPP_VERSION=vX.Y.Z); local
# builds report "dev", which disables the in-app self-update download path.
set(APP_VERSION "dev" CACHE STRING "Release version (CI passes the git tag)")

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Qml Network Svg Test)

# Optional: the guide's moving preview of what is on a channel. Optional on
# purpose — it is one panel on one screen, and neither the build nor the app
# should fail for want of it. The QML that uses it is loaded through a Loader
# so a build or an install without the module simply shows a still card
# instead. (Debian: qt6-multimedia-dev, qml6-module-qtmultimedia.)
find_package(Qt6 QUIET COMPONENTS Multimedia)

# Optional: Qt's Sql module backs the local library index (LibraryDb). Optional
# on purpose: the module's own guard macro APP_HAVE_SQLITE is what the source is
# written against, so a build without the Sql package still links and runs, it
# just has no persistent index and answers "unknown" from the DB. (Debian:
# libqt6sql6 + libqt6sql6-sqlite, the latter carrying the QSQLITE plugin.)
find_package(Qt6 QUIET COMPONENTS Sql)
if(Qt6Sql_FOUND)
    add_compile_definitions(APP_HAVE_SQLITE)
endif()

find_package(OpenSSL REQUIRED)
find_package(SDL2 CONFIG REQUIRED) # gamepad input (InputManager)

# In-process YouTube Innertube resolver (vendor). GPL-3.0; see
# third_party/innertube-qt/ for provenance and the two CMake patches below.
# Built with INNERTUBE_NO_WEBENGINE=ON — the auth store's WebEngine includes
# are guarded, and the app never needs to render a web page itself.
set(INNERTUBE_NO_WEBENGINE ON CACHE BOOL "Disable WebEngine for innertube-qt" FORCE)
add_subdirectory(third_party/innertube-qt)

# libdrm is used on Linux/RPi to save and restore the KMS CRTC state
# around mpv playback so Qt EGLFS can resume cleanly after mpv exits.
if(NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Libdrm REQUIRED libdrm)
endif()

# PC/SC for the NFC reader module. Optional: the module's other driver talks to
# a PN532 over USB serial through plain termios, so readers still work without
# it — which is what makes the NFC module usable on immutable distros where
# pcscd cannot be installed. macOS ships PCSC.framework with the OS; Linux needs
# libpcsclite-dev.
if(APPLE)
    find_library(PCSC_LIBRARIES PCSC)
    if(PCSC_LIBRARIES)
        set(PCSC_FOUND TRUE)
    endif()
else()
    pkg_check_modules(PCSC IMPORTED_TARGET libpcsclite)
endif()
if(NOT PCSC_FOUND)
    message(STATUS "PC/SC not found — NFC reader module will build with PN532 USB serial support only")
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

add_executable(240mp
    src/main.cpp
    src/AppCore.cpp
    src/util/ConfigFile.cpp
    src/modules/local_files/LocalFilesBackend.cpp
    src/modules/plex/PlexBackend.cpp
    src/modules/jellyfin/JellyfinBackend.cpp
    src/modules/emby/EmbyBackend.cpp
    src/modules/ambient_mode/AmbientModeBackend.cpp
    src/modules/youtube/YouTubeBackend.cpp
    src/modules/youtube/YtInnertubeResolver.cpp
    src/modules/nfc_reader/NfcReaderBackend.cpp
    src/modules/nfc_reader/PcscDriver.cpp
    src/modules/nfc_reader/Pn532SerialDriver.cpp
    src/modules/nfc_reader/SerialPort.cpp
    src/modules/weather/WeatherBackend.cpp
    src/modules/scripts/ScriptsBackend.cpp
    src/modules/scripts/ScriptLauncher.cpp
    src/modules/retroarch/RetroArchBackend.cpp
    src/modules/retroarch/RecentGamesStore.cpp
    src/modules/virtual_channels/VirtualChannelsBackend.cpp
    src/modules/virtual_channels/ChannelSchedule.cpp
    src/modules/virtual_channels/ChannelTuner.cpp
    src/modules/virtual_channels/TrackChoice.cpp
    src/modules/virtual_channels/PathGuard.cpp
    src/modules/virtual_channels/ScheduleGenerator.cpp
    src/modules/virtual_channels/DurationProbe.cpp
    src/modules/virtual_channels/MediaServerSource.cpp
    src/modules/virtual_channels/LocalLibrary.cpp
    src/modules/virtual_channels/LibraryDb.cpp
    src/modules/virtual_channels/Catalog.cpp
    src/modules/virtual_channels/TVDBAuth.cpp
    src/modules/virtual_channels/TVDBNetworkResolver.cpp
    src/player/MpvController.cpp
    src/player/SeekHoldPolicy.cpp
    src/util/MpvBasicOptions.cpp
    src/util/MpvLocator.cpp
    src/util/ExecPath.cpp
    src/util/DisplayHandoff.cpp
    src/util/FontconfigOverride.cpp
    src/audio/NoopVolumeMixer.cpp
    src/audio/PipeWireVolumeMixer.cpp
    src/audio/ALSAVolumeMixer.cpp
    src/audio/VolumeMixerFactory.cpp
    src/audio/VolumeManager.cpp
    src/audio/MixerProcessRunner.cpp
    src/input/InputManager.cpp
    src/input/IdleTracker.cpp
    src/update/UpdateManager.cpp
    $<$<BOOL:${APPLE}>:src/util/MacosUtils.mm>
)

# App target wears the same warning gate the test targets carry: a full -Wall
# -Wextra sweep found 0 errors / 6 findings in 30k LOC, so the gate is cheap
# permanent teeth (future regressions show up in CI logs) — deliberately NOT
# -Werror: the six findings are real cleanup, not build-breakers.
target_compile_options(240mp PRIVATE -Wall -Wextra)

# Per-TU, NOT target-level: the CI value is the commit/tag sha, and a target-wide
# -DAPP_VERSION changes the compile line of EVERY TU on every push, which busts
# ccache entirely (zero hits across pushes - the ccache lane was dead weight).
# Only the two TUs that reference APP_VERSION see it; everything else caches.
set(APP_VERSION_DEF "APP_VERSION=\"${APP_VERSION}\"")
set_property(SOURCE src/main.cpp
                 APPEND PROPERTY COMPILE_DEFINITIONS ${APP_VERSION_DEF})
set_property(SOURCE src/modules/weather/WeatherBackend.cpp
             APPEND PROPERTY COMPILE_DEFINITIONS ${APP_VERSION_DEF})

if(Qt6Multimedia_FOUND)
    target_link_libraries(240mp PRIVATE Qt6::Multimedia)
    message(STATUS "Qt Multimedia found — the guide will show moving previews")
else()
    message(STATUS "Qt Multimedia not found — the guide will show still cards")
endif()

# The library index links the Sql module only when the distro shipped it;
# without it LibraryDb compiles to its unavailable-shim branch and the add-flow
# lists fall back to the catalog path.
if(Qt6Sql_FOUND)
    target_link_libraries(240mp PRIVATE Qt6::Sql)
    message(STATUS "Qt Sql found — the library index is persistent")
else()
    message(STATUS "Qt Sql not found — library presence lives in memory only")
endif()

target_link_libraries(240mp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Network
    Qt6::Svg
    OpenSSL::Crypto
    SDL2::SDL2
    innertube-qt
    $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>
)

if(APPLE)
    target_link_libraries(240mp PRIVATE "-framework AppKit")
endif()

if(PCSC_FOUND)
    target_compile_definitions(240mp PRIVATE MP240_NFC_PCSC_AVAILABLE=1)
    if(APPLE)
        target_link_libraries(240mp PRIVATE ${PCSC_LIBRARIES})
    else()
        target_link_libraries(240mp PRIVATE PkgConfig::PCSC)
    endif()
endif()

if(NOT APPLE)
    target_include_directories(240mp PRIVATE ${Libdrm_INCLUDE_DIRS})
endif()

target_include_directories(240mp PRIVATE
    src
)

# ── macOS bundle metadata ──────────────────────────────────────────────────────
set_target_properties(240mp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "240-MP"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.240mp.app"
    MACOSX_BUNDLE_ICON_FILE "packaging/macos/240-mp"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

# ── Dev bundle symlink (macOS only, non-CI) ───────────────────────────────────
# Makes double-clicking build/240mp.app work during development by symlinking
# Contents/Resources → repo root so resolveAppRoot() finds the QML files.
# Skipped in CI (ENV{CI} is set by GitHub Actions) because cmake --install
# populates Resources with real files — a symlink in the build tree would be
# packaged into the DMG pointing to a CI path that doesn't exist on end-user Macs.
if(APPLE AND NOT DEFINED ENV{CI})
    add_custom_command(TARGET 240mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove -f
            "$<TARGET_BUNDLE_CONTENT_DIR:240mp>/Resources"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_SOURCE_DIR}"
            "$<TARGET_BUNDLE_CONTENT_DIR:240mp>/Resources"
        COMMENT "Symlinking repo root as bundle Resources (dev build)"
    )
endif()

# ── Install rules ──────────────────────────────────────────────────────────────
if(APPLE)
    install(TARGETS 240mp BUNDLE DESTINATION . COMPONENT Runtime)

    # Use a hardcoded relative path — $<TARGET_BUNDLE_CONTENT_DIR> resolves to
    # the build tree, not the install prefix, inside install() rules.
    install(FILES "${CMAKE_SOURCE_DIR}/Main.qml"
        DESTINATION "240mp.app/Contents/Resources"
        COMPONENT Runtime)

    # Keep third-party licences under their own paths: several are named LICENSE.
    install(FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/THANKS.md"
        DESTINATION "240mp.app/Contents/Resources"
        COMPONENT Runtime)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/LICENSE")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/LICENSE"
            DESTINATION "240mp.app/Contents/Resources/third_party/innertube-qt"
            COMPONENT Runtime)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/lib/verdigris/LICENSE.LGPLv3")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/lib/verdigris/LICENSE.LGPLv3"
            DESTINATION "240mp.app/Contents/Resources/third_party/innertube-qt/lib/verdigris"
            COMPONENT Runtime)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/expected-lite/LICENSE.txt")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/expected-lite/LICENSE.txt"
            DESTINATION "240mp.app/Contents/Resources/third_party/expected-lite"
            COMPONENT Runtime)
    endif()

    install(DIRECTORY
        "${CMAKE_SOURCE_DIR}/views"
        "${CMAKE_SOURCE_DIR}/modules"
        "${CMAKE_SOURCE_DIR}/assets"
        "${CMAKE_SOURCE_DIR}/scripts"
        DESTINATION "240mp.app/Contents/Resources"
        COMPONENT Runtime)

    # Icon lives under packaging/macos (not assets/) so it ships alongside the
    # other macOS packaging inputs; installed at the same relative path so it
    # keeps matching MACOSX_BUNDLE_ICON_FILE above.
    install(DIRECTORY
        "${CMAKE_SOURCE_DIR}/packaging/macos"
        DESTINATION "240mp.app/Contents/Resources/packaging"
        COMPONENT Runtime)

    # macdeployqt is NOT run here — it requires Qt frameworks to be present and
    # is environment-specific. Run it manually in CI after cmake --install.
else()
    install(TARGETS 240mp
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Runtime)

    install(FILES "${CMAKE_SOURCE_DIR}/Main.qml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp"
        COMPONENT Runtime)

    # Keep third-party licences under their own paths: several are named LICENSE.
    install(FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/THANKS.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp"
        COMPONENT Runtime)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/LICENSE")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp/third_party/innertube-qt"
            COMPONENT Runtime)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/lib/verdigris/LICENSE.LGPLv3")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/innertube-qt/lib/verdigris/LICENSE.LGPLv3"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp/third_party/innertube-qt/lib/verdigris"
            COMPONENT Runtime)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/expected-lite/LICENSE.txt")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/expected-lite/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp/third_party/expected-lite"
            COMPONENT Runtime)
    endif()

    install(DIRECTORY
        "${CMAKE_SOURCE_DIR}/views"
        "${CMAKE_SOURCE_DIR}/modules"
        "${CMAKE_SOURCE_DIR}/assets"
        "${CMAKE_SOURCE_DIR}/scripts"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/240mp"
        COMPONENT Runtime)
endif()

# ── Tests (opt-in) ─────────────────────────────────────────────────────────────
# OFF by default: the normal build and the release pipeline are unaffected.
# QtCore only — these cover pure logic (playlist parsing, config loading) and
# need no Quick, no mpv and no display, so they run headless on any target.
#
#   cmake -B build -DBUILD_TESTS=ON
#   cmake --build build --target <target> && ./build/<target>
option(BUILD_TESTS "Build the unit test suite" OFF)
if(BUILD_TESTS)
    add_executable(retroarch-tests
        src/modules/retroarch/tests/RetroArchBackendTest.cpp
        src/modules/retroarch/RetroArchBackend.cpp
        src/util/ConfigFile.cpp
        # RetroArchBackend.cpp references DisplayHandoff for the launch path and
        # RecentGamesStore for the recently-played bookkeeping; the tests never
        # launch, but the symbols must resolve to link.
        src/util/DisplayHandoff.cpp
        src/modules/retroarch/RecentGamesStore.cpp)
    set_target_properties(retroarch-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(retroarch-tests PRIVATE Qt6::Core Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    if(NOT APPLE)
        target_include_directories(retroarch-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(retroarch-tests PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME retroarch-backend COMMAND retroarch-tests)

    # Ambient Mode backend: filesystem scans, settings fallback and companion
    # audio process lifecycle. QtCore only; a fake `mpv` on PATH stands in for
    # the real player so the suite never touches audio hardware.
    add_executable(ambient-mode-tests
        src/modules/ambient_mode/tests/AmbientModeBackendTest.cpp
        src/modules/ambient_mode/AmbientModeBackend.cpp
        src/util/ConfigFile.cpp
        src/util/MpvLocator.cpp)
    set_target_properties(ambient-mode-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(ambient-mode-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(ambient-mode-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ambient-mode-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ambient-mode COMMAND ambient-mode-tests)

    # pi-rc decide-mode regression (issue #91): pure shell logic, no Qt/display.
    # Pins that a closed-unmerged PR with a matching pr-N marker selects MODE=restore
    # instead of silently skipping. Runs in both CI lanes via the existing ctest steps.
    add_test(NAME pi-rc-decide COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pi-rc-decide-test.sh)

    # Recently-played / playlist-position bookkeeping (pure logic, no QObject).
    # Lives beside the RetroArch tests because it is the same module's state, but
    # as its own target so it cannot be broken by anything that needs a display.
    add_executable(recent-games-tests
        src/modules/retroarch/tests/RecentGamesStoreTest.cpp
        src/modules/retroarch/RecentGamesStore.cpp)
    set_target_properties(recent-games-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(recent-games-tests PRIVATE Qt6::Core Qt6::Test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(recent-games-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME recent-games COMMAND recent-games-tests)

    # Weather code-mapping tests: pure table lookups and METAR reduction, no
    # files, no network. WeatherCodes.h is self-contained, so this target
    # needs nothing beyond Qt Test.
    add_executable(weather-tests
        src/modules/weather/tests/WeatherCodesTest.cpp)
    set_target_properties(weather-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(weather-tests PRIVATE Qt6::Core Qt6::Test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(weather-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME weather-codes COMMAND weather-tests)

    # WeatherBackend against a loopback fake of all four weather APIs. Env-var
    # endpoint overrides (the same seam UpdateManager uses for its feed) make
    # races and stale-cache cases reproducible without touching the network.
    add_executable(weather-backend-tests
        src/modules/weather/tests/WeatherBackendTest.cpp
        src/modules/weather/WeatherBackend.cpp
        src/util/ConfigFile.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp)
    set_target_properties(weather-backend-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(weather-backend-tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test)
    target_include_directories(weather-backend-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(weather-backend-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME weather-backend COMMAND weather-backend-tests)

    # AppCore -> weather module proxy (issue #53): app-level views load outside
    # the weather module's QQml scope where the module-scope "weatherBackend"
    # context property is null. The location reads are proxied through appCore,
    # which holds every backend. Links AppCore + WeatherBackend and exercises
    # the proxy directly (no QML engine, no network).
    add_executable(appcore-weather-proxy-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/modules/weather/WeatherBackend.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/tests/AppCoreWeatherProxyTest.cpp)
    set_target_properties(appcore-weather-proxy-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(appcore-weather-proxy-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test)
    target_include_directories(appcore-weather-proxy-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(appcore-weather-proxy-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME appcore-weather-proxy COMMAND appcore-weather-proxy-tests)

    # Issue #85: AppCore::save_setting must push a setting into backends before
    # it emits moduleSettingChanged, because view handlers refresh on that
    # signal and read the backend immediately. Uses real AppCore + real
    # RetroArchBackend; no QML, no network (QGuiApplication for offscreen Qt).
    add_executable(appcore-setting-order-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/modules/retroarch/RetroArchBackend.cpp
        src/modules/retroarch/RecentGamesStore.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/tests/AppCoreSettingOrderTest.cpp)
    set_target_properties(appcore-setting-order-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(appcore-setting-order-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(appcore-setting-order-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(appcore-setting-order-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(appcore-setting-order-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME appcore-setting-order COMMAND appcore-setting-order-tests)

    # T1 M-3: re-registering the same backend/module id must not multiply
    # AppCore's signal fan-out. Real AppCore + probe backend, QSignalSpy on
    # all three re-emitted signals; control (x1) and stress (x10) arms. The
    # stress arm FAILS BY DESIGN until registerModule() is idempotent.
    # No QML engine, no network (QGuiApplication for offscreen Qt).
    add_executable(appcore-reregistration-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/util/MpvBasicOptions.cpp
        src/tests/AppCoreReregistrationTest.cpp)
    set_target_properties(appcore-reregistration-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(appcore-reregistration-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test)
    target_include_directories(appcore-reregistration-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(appcore-reregistration-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME appcore-reregistration COMMAND appcore-reregistration-tests)

    # Raw keyboard/remote taps must be visible in the TELEMETRY state log too.
    # Uses the real InputManager event filter and AppCore log sink; no QML,
    # network, audio hardware, or physical input device is driven.
    add_executable(input-manager-telemetry-tests
        src/input/InputManager.cpp
        src/AppCore.cpp
        src/audio/VolumeManager.cpp
        src/audio/NoopVolumeMixer.cpp
        src/audio/PipeWireVolumeMixer.cpp
        src/audio/ALSAVolumeMixer.cpp
        src/audio/MixerProcessRunner.cpp
        src/audio/VolumeMixerFactory.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/ConfigFile.cpp
        src/tests/InputManagerTelemetryTest.cpp)
    set_target_properties(input-manager-telemetry-tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(input-manager-telemetry-tests PRIVATE
        APP_TELEMETRY MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(input-manager-telemetry-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test SDL2::SDL2
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(input-manager-telemetry-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(input-manager-telemetry-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(input-manager-telemetry-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME input-manager-telemetry COMMAND input-manager-telemetry-tests)

    # IdleTracker blocking flags (IDLE-02): gameActive must freeze the idle
    # count exactly like mpvActive/scriptActive, and the game-end reset pairing
    # must give the keyless return a full fresh threshold. Accelerated 2 s
    # threshold with real 1 s ticks; no display, no processes (QtGui only for
    # the QKeyEvent type the event filter handles).
    add_executable(idle-tracker-tests
        src/tests/IdleTrackerTest.cpp
        src/input/IdleTracker.cpp)
    set_target_properties(idle-tracker-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(idle-tracker-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(idle-tracker-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME idle-tracker COMMAND idle-tracker-tests)

    # System-volume mixer abstraction: exercises the factory's backend
    # detection (PipeWire > ALSA > no-op) and the no-op fallback's contract.
    # No real audio hardware needed — the factory picks a no-op on WSL/CI, and
    # the async ALSA backend is driven end-to-end with the compiled mixer-stub
    # child (TORTURE_* env knobs) through the real MixerProcessRunner seam.
    add_executable(volume-mixer-tests
        src/audio/NoopVolumeMixer.cpp
        src/audio/PipeWireVolumeMixer.cpp
        src/audio/ALSAVolumeMixer.cpp
        src/audio/MixerProcessRunner.cpp
        src/audio/VolumeMixerFactory.cpp
        src/tests/VolumeMixerTest.cpp)
    set_target_properties(volume-mixer-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(volume-mixer-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(volume-mixer-tests PRIVATE src)
    target_compile_definitions(volume-mixer-tests PRIVATE
        MIXER_STUB_PATH="$<TARGET_FILE:mixer-stub>")
    add_dependencies(volume-mixer-tests mixer-stub)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(volume-mixer-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME volume-mixer COMMAND volume-mixer-tests)

    # App-wide volume owner (#34): the state machine above the mixers.
    add_executable(volume-manager-tests
        src/audio/VolumeManager.cpp
        src/audio/NoopVolumeMixer.cpp
        src/audio/PipeWireVolumeMixer.cpp
        src/audio/ALSAVolumeMixer.cpp
        src/audio/MixerProcessRunner.cpp
        src/audio/VolumeMixerFactory.cpp
        src/tests/VolumeManagerTest.cpp)
    set_target_properties(volume-manager-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(volume-manager-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(volume-manager-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(volume-manager-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME volume-manager COMMAND volume-manager-tests)

    # Async mixer process seam (T1 P-5A): dedicated-thread QProcess runner with
    # latest-wins coalescing, bounded timeouts and cached-state accessors. A
    # fake child executable stands in for amixer/wpctl (delay, exit codes,
    # garbage output) so the suite needs no audio hardware. Run under
    # ASan/UBSan with detect_leaks=1 — clean shutdown is part of the contract.
    add_executable(mixer-fake-child src/tests/fake-mixer-child.cpp)
    add_executable(mixer-process-runner-tests
        src/audio/MixerProcessRunner.cpp
        src/tests/MixerProcessRunnerTest.cpp)
    set_target_properties(mixer-process-runner-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mixer-process-runner-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(mixer-process-runner-tests PRIVATE src)
    target_compile_definitions(mixer-process-runner-tests PRIVATE
        FAKE_MIXER_CHILD_PATH="$<TARGET_FILE:mixer-fake-child>")
    add_dependencies(mixer-process-runner-tests mixer-fake-child)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mixer-process-runner-tests PRIVATE -Wall -Wextra)
        target_compile_options(mixer-fake-child PRIVATE -Wall -Wextra)
    endif()

    # Held-key seek shaping (YouTube slow-stream fix): pure policy logic, no
    # mpv/IPC/timers needed — the owner drives flush() from a QTimer.
    add_executable(seek-hold-policy-tests
        src/player/SeekHoldPolicy.cpp
        src/tests/SeekHoldPolicyTest.cpp)
    set_target_properties(seek-hold-policy-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(seek-hold-policy-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(seek-hold-policy-tests PRIVATE src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(seek-hold-policy-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME seek-hold-policy COMMAND seek-hold-policy-tests)
    add_test(NAME mixer-process-runner COMMAND mixer-process-runner-tests)

    # PipeWire async backend contract (T1 P-5A): drives the real backend against
    # the checked-in compiled mixer-stub on PATH (no audio hardware). Timing,
    # coalescing, clamp no-ops and every failure-mode rollback, sanitizer-clean.
    add_executable(pipewire-volume-mixer-tests
        src/audio/PipeWireVolumeMixer.cpp
        src/audio/MixerProcessRunner.cpp
        src/tests/PipeWireVolumeMixerTest.cpp)
    set_target_properties(pipewire-volume-mixer-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(pipewire-volume-mixer-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(pipewire-volume-mixer-tests PRIVATE src)
    add_dependencies(pipewire-volume-mixer-tests mixer-stub)
    target_compile_definitions(pipewire-volume-mixer-tests PRIVATE
        MIXER_STUB_PATH="$<TARGET_FILE:mixer-stub>")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pipewire-volume-mixer-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME pipewire-volume-mixer COMMAND pipewire-volume-mixer-tests)

    # P-5A delayed-mixer UI-stall regression (torture T1): a slow system mixer
    # must not stall the UI thread >16 ms. Drives the REAL InputManager file
    # channel + REAL VolumeManager + REAL ALSA/PipeWire mixers offscreen, with
    # scratch PATH-stub amixer/wpctl whose delay/failure mode the orchestrator
    # controls (TORTURE_DELAY_MS / TORTURE_STUB_MODE). No audio hardware, no
    # system state: all delay and state live in scratch dirs. Two-sided: the
    # delay200 cases and alsa-delay0 FAIL on the unfixed snapshot (ALSA ~1.5 s
    # direct / ~1.5 s heartbeat, 7 amixer spawns per press; PipeWire ~211 ms);
    # noop-control proves the rig itself is green with an instant mixer, and
    # the fault cases (nonzero exit / garbage / missing executable) must stay
    # bounded and sanitizer-clean. Needs python3 (stdlib only). RUN_SERIAL:
    # the runs share the app's /tmp/240mp_key.in debug channel.
    #   cmake -B build-test -DBUILD_TESTS=ON
    #   cmake --build build-test --target volume-stall-probe mixer-stub
    #   ctest --test-dir build-test -R volume-stall --output-on-failure
    # See src/tests/README-volume-stall.md (env vars, RESULT fields, baseline).
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # The PATH-stub mixer: one compiled binary the orchestrator copies into
    # scratch bin dirs as amixer/wpctl. Compiled, not scripted — interpreter
    # startup would dwarf the ~3 ms spawn cost the baseline was measured
    # with. Built WITHOUT sanitizers even in sanitizer builds (it is fixture
    # infrastructure, not product code; ASan startup would skew the timings).
    add_executable(mixer-stub src/tests/mixer-stubs/mixer_stub.cpp)
    set_target_properties(mixer-stub PROPERTIES AUTOMOC OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mixer-stub PRIVATE -Wall -Wextra
            -fno-sanitize=address,undefined)
        target_link_options(mixer-stub PRIVATE -fno-sanitize=address,undefined)
    endif()
    add_executable(volume-stall-probe
        src/tests/VolumeStallProbe.cpp
        # Same link closure the lab harness proved for InputManager+AppCore:
        # the input layer references MpvController (media-key IPC) and the
        # RetroArch backend (launch handoff), so their symbols must resolve.
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/input/InputManager.cpp
        src/modules/retroarch/RetroArchBackend.cpp
        src/modules/retroarch/RecentGamesStore.cpp
        src/audio/VolumeManager.cpp
        src/audio/NoopVolumeMixer.cpp
        src/audio/PipeWireVolumeMixer.cpp
        src/audio/ALSAVolumeMixer.cpp
        src/audio/MixerProcessRunner.cpp
        src/audio/VolumeMixerFactory.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/ExecPath.cpp
        src/util/DisplayHandoff.cpp
        src/util/FontconfigOverride.cpp)
    set_target_properties(volume-stall-probe PROPERTIES AUTOMOC ON)
    target_link_libraries(volume-stall-probe PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick SDL2::SDL2
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(volume-stall-probe PRIVATE src)
    if(NOT APPLE)
        target_include_directories(volume-stall-probe PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    # Repo root as a C string literal (same set()/\" pattern as the smokes:
    # a bare inline \" gets stripped by target_compile_definitions).
    set(MP240_VOLUME_STALL_ROOT_DEF "MP240_REPO_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    target_compile_definitions(volume-stall-probe PRIVATE ${MP240_VOLUME_STALL_ROOT_DEF})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(volume-stall-probe PRIVATE -Wall -Wextra)
    endif()
    foreach(stall_case alsa-delay200 alsa-delay0 pipewire-delay200 pipewire-delay0
                       noop-control alsa-faults pipewire-faults alsa-edge pipewire-edge)
        # The 16 ms wall-clock gate needs a quiet host. The macOS CI runner
        # (macos-15-intel) jitters 50-58 ms on the noop control's heartbeat
        # with ZERO mixer I/O — rig noise, not product (the canary's exact
        # verdict, see README). Timing cases therefore run on Linux only;
        # fault cases (boundedness/sanitizer, no 16 ms gate) run everywhere.
        if(APPLE AND NOT stall_case MATCHES "faults$")
            continue()
        endif()
        # Both mixers are async on the mixer process seam (T1 P-5A), so
        # every case asserts the fixed shape in normal mode. --baseline
        # remains an orchestrator mode for the two-sided red-shape proof.
        add_test(NAME volume-stall-${stall_case}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/run_volume_stall_case.py
                --case ${stall_case}
                --probe $<TARGET_FILE:volume-stall-probe>
                --stub-bin $<TARGET_FILE:mixer-stub>
                --repo-root ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(volume-stall-${stall_case} PROPERTIES
            TIMEOUT 180
            RUN_SERIAL TRUE
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    # IDLE-01 stale idle-count regression gate (torture IDLE-01): a keyless
    # return from playback/script must start a FULL idle period, not fire the
    # saver on the first tick from the frozen pre-playback count. Drives the
    # REAL IdleTracker offscreen (Qt Core+Gui only — no QML/mpv/audio, runs on
    # every CI lane) with an accelerated 4 s threshold and real 1000 ms ticks.
    # Two-sided: the mpv and script arms FAIL by design on the pre-fix class
    # (first-tick fire ~0.5 s / 1 tick); keypress and fresh-idle controls PASS
    # on both sides. Every arm asserts the freeze invariant (zero
    # idleSecondsChanged emissions during a 3.5 s block). Needs python3
    # (stdlib only). RUN_SERIAL: timing gate, keep the 1 s ticks quiet.
    #   cmake -B build-test -DBUILD_TESTS=ON
    #   cmake --build build-test --target idle-tracker-stale-count-probe
    #   ctest --test-dir build-test -R idle-tracker-stale-count --output-on-failure
    # Two-sided control build (pre-fix checkout, never edited):
    #   cmake -B build-ctl -DBUILD_TESTS=ON -DIDLE01_SOURCE_ROOT=<pre-fix-tree>
    #   cmake --build build-ctl --target idle-tracker-stale-count-probe
    #   ctest --test-dir build-ctl -R idle-tracker-stale-count --output-on-failure
    # See src/tests/README-idle-tracker-stale-count.md (env, bands, expectations).
    set(IDLE01_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
        "Source tree whose src/input/IdleTracker.cpp the stale-count probe compiles (point at a pre-fix checkout for the two-sided control run)")
    add_executable(idle-tracker-stale-count-probe
        src/tests/IdleTrackerStaleCountProbe.cpp
        ${IDLE01_SOURCE_ROOT}/src/input/IdleTracker.cpp)
    set_target_properties(idle-tracker-stale-count-probe PROPERTIES AUTOMOC ON)
    target_include_directories(idle-tracker-stale-count-probe PRIVATE
        ${IDLE01_SOURCE_ROOT}/src/input)
    target_link_libraries(idle-tracker-stale-count-probe PRIVATE Qt6::Core Qt6::Gui)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(idle-tracker-stale-count-probe PRIVATE -Wall -Wextra)
    endif()
    # Compiled-rev provenance: the JSON reports the tree the class came FROM
    # (IDLE01_SOURCE_ROOT), not the configure tree — they differ in control
    # builds. Same set()/\" pattern as the other root defs.
    execute_process(COMMAND git -C "${IDLE01_SOURCE_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE IDLE01_SOURCE_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        RESULT_VARIABLE IDLE01_REV_RC)
    if(NOT IDLE01_REV_RC EQUAL 0)
        set(IDLE01_SOURCE_REV "unknown")
    endif()
    set(IDLE01_REV_DEF "IDLE01_SOURCE_REV=\"${IDLE01_SOURCE_REV}\"")
    target_compile_definitions(idle-tracker-stale-count-probe PRIVATE ${IDLE01_REV_DEF})
    foreach(idle_case mpv script keypress fresh)
        add_test(NAME idle-tracker-stale-count-${idle_case}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/run_idle_tracker_stale_count_case.py
                --case ${idle_case}
                --probe $<TARGET_FILE:idle-tracker-stale-count-probe>
                --source-root ${IDLE01_SOURCE_ROOT})
        set_tests_properties(idle-tracker-stale-count-${idle_case} PROPERTIES
            TIMEOUT 300
            RUN_SERIAL TRUE
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    # Plex playback-decision logic (header-only — links nothing else).
    add_executable(plex-playback-decision
        src/modules/plex/tests/PlaybackDecisionTest.cpp)
    set_target_properties(plex-playback-decision PROPERTIES AUTOMOC ON)
    target_link_libraries(plex-playback-decision PRIVATE Qt6::Core Qt6::Test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(plex-playback-decision PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME plex-playback-decision COMMAND plex-playback-decision)
endif()
# OFF by default: the normal build and the release pipeline are unaffected, and
# an upstream PR is not forced to adopt a test framework. QtCore only — these
# cover the scheduling arithmetic, which needs no Quick, no mpv and no display.
#
#   cmake -B build -DBUILD_VCHAN_TESTS=ON
#   cmake --build build --target vchan-tests && ./build/vchan-tests
# ── CFG-01 regression gates (config.json atomicity + corrupt-load policy) ──
# Two-sided gates per test plan t_d4e98f0b. Tier-1 sources compile against
# PRE/P2/FIX snapshots alike (cfg01::saveSettingShim bridges void->bool).
# Enable with -DBUILD_CFG01_TESTS=ON.
option(BUILD_CFG01_TESTS "Build the CFG-01 config-file regression gates" OFF)
if(BUILD_CFG01_TESTS)
    set(CFG01_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/cfg01)
    # Real AppCore TUs from THIS snapshot — proven minimal set, identical to
    # appcore-setting-order-tests. ConfigFile.cpp exists only on the FIX side.
    set(CFG01_CORE_SOURCES
        src/AppCore.cpp
        src/modules/retroarch/RetroArchBackend.cpp
        src/modules/retroarch/RecentGamesStore.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ConfigFile.cpp)
        list(APPEND CFG01_CORE_SOURCES src/util/ConfigFile.cpp)
    endif()
    set(CFG01_CORE_LIBS Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test)
    if(NOT APPLE)
        list(APPEND CFG01_CORE_LIBS ${Libdrm_LIBRARIES})
    endif()

    # Classifier v3: QtCore ONLY (no product code) — §4.1.
    add_executable(cfg01-classify ${CFG01_DIR}/cfg01_classify.cpp)
    target_link_libraries(cfg01-classify PRIVATE Qt6::Core)
    target_include_directories(cfg01-classify PRIVATE ${CFG01_DIR})

    # Killprobe: pure POSIX timing path, no Qt anywhere near the kill clock.
    add_executable(cfg01-killprobe ${CFG01_DIR}/cfg01_killprobe.cpp)

    add_executable(cfg01-saver ${CFG01_DIR}/cfg01_saver.cpp ${CFG01_CORE_SOURCES})
    set_target_properties(cfg01-saver PROPERTIES AUTOMOC ON)
    target_include_directories(cfg01-saver PRIVATE src ${CFG01_DIR})
    target_link_libraries(cfg01-saver PRIVATE ${CFG01_CORE_LIBS})
    if(NOT APPLE)
        target_include_directories(cfg01-saver PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()

    add_executable(cfg01-harness ${CFG01_DIR}/cfg01_harness.cpp ${CFG01_CORE_SOURCES})
    set_target_properties(cfg01-harness PROPERTIES AUTOMOC ON)
    target_include_directories(cfg01-harness PRIVATE src ${CFG01_DIR})
    target_link_libraries(cfg01-harness PRIVATE ${CFG01_CORE_LIBS})
    if(NOT APPLE)
        target_include_directories(cfg01-harness PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()

    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cfg01-kill-sweep
        COMMAND ${Python3_EXECUTABLE} ${CFG01_DIR}/run_kill_sweep.py
            --saver $<TARGET_FILE:cfg01-saver>
            --killprobe $<TARGET_FILE:cfg01-killprobe>
            --classify $<TARGET_FILE:cfg01-classify>
            --evdir ${CMAKE_CURRENT_BINARY_DIR}/cfg01-evidence/sweep
            --golden-dir ${CFG01_DIR}/golden
            # Test-plan 193: grid must be calibrated on the host/PRE lane to
            # bracket the FIX commit window. On this host 1550 us never lands
            # a post-commit kill; 12000 us is the calibrated grid (1205 real
            # SIGKILLs, zero corruption, G1/G3 pass). This is a sweep timing
            # grid, not a corruption threshold — none of the gates are relaxed.
            --max-us 12000
            --variant FIX --qt-version ${Qt6Core_VERSION})
    set_tests_properties(cfg01-kill-sweep PROPERTIES TIMEOUT 900)
    foreach(_m healthy corrupt-refusal storm missing-unreadable open-failure)
        add_test(NAME cfg01-${_m}
            COMMAND cfg01-harness ${_m} ${CMAKE_CURRENT_BINARY_DIR}/cfg01-evidence/${_m})
        set_tests_properties(cfg01-${_m} PROPERTIES TIMEOUT 120 ENVIRONMENT QT_QPA_PLATFORM=offscreen)
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME cfg01-armB
            COMMAND cfg01-harness armB ${CMAKE_CURRENT_BINARY_DIR}/cfg01-evidence/armB)
        set_tests_properties(cfg01-armB PROPERTIES TIMEOUT 60 ENVIRONMENT QT_QPA_PLATFORM=offscreen)
    endif()

    # N4a: credential-file atomicity gates. Header-only production path +
    # TVDBAuth.cpp (its only non-header writer). QtCore only — no app shell.
    add_executable(cfg01-auth-atomic ${CFG01_DIR}/auth_atomic_test.cpp
                   src/modules/virtual_channels/TVDBAuth.cpp)
    target_include_directories(cfg01-auth-atomic PRIVATE src)
    target_link_libraries(cfg01-auth-atomic PRIVATE Qt6::Core)
    add_test(NAME cfg01-auth-atomic COMMAND cfg01-auth-atomic)
    set_tests_properties(cfg01-auth-atomic PROPERTIES TIMEOUT 180)
endif()

option(BUILD_VCHAN_TESTS "Build the virtual-channels scheduling tests" OFF)
if(BUILD_VCHAN_TESTS)
    add_executable(vchan-tests
        src/modules/virtual_channels/tests/main.cpp
        src/modules/virtual_channels/tests/ChannelScheduleTest.cpp
        src/modules/virtual_channels/tests/PathGuardTest.cpp
        src/modules/virtual_channels/tests/ChannelTunerTest.cpp
        src/modules/virtual_channels/tests/TrackChoiceTest.cpp
        src/modules/virtual_channels/tests/ScheduleGeneratorTest.cpp
        src/modules/virtual_channels/tests/DurationProbeTest.cpp
        src/modules/virtual_channels/tests/MediaServerSourceTest.cpp
        src/modules/virtual_channels/tests/LocalLibraryTest.cpp
        src/modules/virtual_channels/tests/VirtualChannelsBackendTest.cpp
        src/modules/virtual_channels/tests/SubjectEngineTest.cpp
        src/modules/virtual_channels/tests/CatalogTest.cpp
        src/modules/virtual_channels/tests/TVDBAuthTest.cpp
        src/modules/virtual_channels/tests/TVDBNetworkResolverTest.cpp
        src/modules/virtual_channels/tests/LibraryDbTest.cpp
        # Listed so AUTOMOC processes it: it declares a QObject but has no
        # same-named source file for AUTOMOC to pair it with.
        src/modules/virtual_channels/tests/FakeMediaServer.h
        src/modules/virtual_channels/tests/FakePlexServer.h
        src/modules/virtual_channels/tests/FakeJellyfinServer.h
        src/modules/virtual_channels/ChannelSchedule.cpp
        src/modules/virtual_channels/PathGuard.cpp
        src/modules/virtual_channels/ChannelTuner.cpp
        src/modules/virtual_channels/TrackChoice.cpp
        src/modules/virtual_channels/ScheduleGenerator.cpp
        src/modules/virtual_channels/DurationProbe.cpp
        src/modules/virtual_channels/MediaServerSource.cpp
        src/modules/virtual_channels/SubjectEngine.cpp
        src/modules/virtual_channels/Catalog.cpp
        src/modules/virtual_channels/TVDBAuth.cpp
        src/modules/virtual_channels/TVDBNetworkResolver.cpp
        src/modules/virtual_channels/LocalLibrary.cpp
        src/modules/virtual_channels/VirtualChannelsBackend.cpp
        src/util/ConfigFile.cpp
        src/modules/virtual_channels/LibraryDb.cpp
        src/modules/virtual_channels/tests/LibraryDbTest.cpp)
    # MediaServerSource and the fake backend it is tested against are both
    # QObjects, so this target needs moc even though the rest of the suite is
    # plain C++.
    set_target_properties(vchan-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(vchan-tests PRIVATE Qt6::Core Qt6::Network)
    if(Qt6Sql_FOUND)
        target_link_libraries(vchan-tests PRIVATE Qt6::Sql)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(vchan-tests PRIVATE -Wall -Wextra)
    endif()
    enable_testing()
    add_test(NAME vchan-schedule COMMAND vchan-tests)

    # Preset add-flow QML smoke test: loads PresetRecipe/PresetSubject offscreen
    # with stubbed backends and asserts the recipe -> rows -> create+nav wiring
    # that pure-logic ctest can't reach. Needs Quick+Qml (QML engine) and Gui
    # (QGuiApplication); runs headless via the offscreen platform set in main().
    add_executable(vchan-preset-smoke
        src/tests/preset-smoke-test.cpp)
    set_target_properties(vchan-preset-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(vchan-preset-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
    target_include_directories(vchan-preset-smoke PRIVATE src)
    # Repo root so the test locates the module views and the shared Components/
    # import path without hardcoding a machine path.
    # Repo root as a C string literal. Use the same set()/\" pattern the
    # APP_VERSION def uses — target_compile_definitions strips a bare \" passed
    # inline, which would otherwise split the path into tokens at compile time.
    set(MP240_REPO_ROOT_DEF "MP240_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    target_compile_definitions(vchan-preset-smoke PRIVATE ${MP240_REPO_ROOT_DEF})
    add_test(NAME vchan-preset-smoke COMMAND vchan-preset-smoke)
endif()

# Recently-played view layout smoke: loads Recent.qml offscreen with stub
# backends and proves the thumbnail pane + letter strip the CRT flagged as
# missing are actually wired, ordered and non-overlapping. Not gated behind
# BUILD_VCHAN_TESTS — this covers the retroarch view shared by every build.
if(BUILD_TESTS)
    add_executable(recent-view-smoke src/tests/recent-view-smoke-test.cpp)
    set_target_properties(recent-view-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(recent-view-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
    target_include_directories(recent-view-smoke PRIVATE src)
    set(MP240_RECENT_ROOT_DEF "MP240_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    target_compile_definitions(recent-view-smoke PRIVATE ${MP240_RECENT_ROOT_DEF})
    add_test(NAME recent-view-smoke COMMAND recent-view-smoke)

    # Current conditions row parity: Metric and US must both show PRESSURE.
    add_executable(weather-current-smoke src/tests/weather-current-smoke-test.cpp)
    set_target_properties(weather-current-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(weather-current-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
    target_include_directories(weather-current-smoke PRIVATE src)
    set(MP240_WEATHER_ROOT_DEF "MP240_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    target_compile_definitions(weather-current-smoke PRIVATE ${MP240_WEATHER_ROOT_DEF})
    add_test(NAME weather-current-smoke COMMAND weather-current-smoke)
    set_tests_properties(weather-current-smoke PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endif()

# Plex Player retry smoke: drives the failed-direct-play -> transcode fallback
# offscreen with stubbed backends and checks mpv gets transcode-shaped tracks.
# Local Files Player smoke: loads the real Player.qml offscreen with stub
# backends and checks the exact flags handed to mpv. Guards the legacy "OFF"
# toggle string and playlist resume with a saved item index but zero time.
if(BUILD_TESTS)
    add_executable(plex-player-smoke src/tests/plex-player-smoke-test.cpp)
    set_target_properties(plex-player-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(plex-player-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
    target_include_directories(plex-player-smoke PRIVATE src)
    set(MP240_PLEX_ROOT_DEF "MP240_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    target_compile_definitions(plex-player-smoke PRIVATE ${MP240_PLEX_ROOT_DEF})
    add_test(NAME plex-player-smoke COMMAND plex-player-smoke)

    # YouTube Player launch smoke: drives the real Player.qml offscreen with
    # stub backends and asserts the launch depends only on the stream URL pair
    # — cached segments ride as an EDL, a still-fetching or dead sponsor domain
    # adds zero wait (the old cold path held first frame for up to 6 s).
    add_executable(youtube-player-smoke src/tests/youtube-player-smoke-test.cpp)
    set_target_properties(youtube-player-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(youtube-player-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
    target_compile_definitions(youtube-player-smoke PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME youtube-player-smoke COMMAND youtube-player-smoke)

    # YouTube Player held-key wiring: repeat flag + key-up must cross to
    # MpvController (SeekHoldPolicy shapes them; logic unit-tested separately).
    add_executable(youtube-seek-smoke src/tests/youtube-seek-smoke-test.cpp)
    set_target_properties(youtube-seek-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(youtube-seek-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
    target_include_directories(youtube-seek-smoke PRIVATE src)
    target_compile_definitions(youtube-seek-smoke PRIVATE MP240_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME youtube-seek-smoke COMMAND youtube-seek-smoke)

    add_executable(local-files-player-smoke src/tests/local-files-player-smoke-test.cpp)
    set_target_properties(local-files-player-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(local-files-player-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
    target_compile_definitions(local-files-player-smoke PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME local-files-player-smoke COMMAND local-files-player-smoke)

    # R10: an empty filePath must goBack() at the launch entry instead of
    # stranding the view (no mpv child ever spawns, so Back/ESC cannot exit).
    # Two-sided: fails on the pre-fix snapshot by design.
    add_executable(local-files-empty-path-smoke src/tests/local-files-empty-path-smoke-test.cpp)
    set_target_properties(local-files-empty-path-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(local-files-empty-path-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
    target_compile_definitions(local-files-empty-path-smoke PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME local-files-empty-path-smoke COMMAND local-files-empty-path-smoke)

    # YouTube primary lists: a real Down key must emit a named select line.
    # Runs on the x86/macOS QML lanes; arm64 excludes it with the other Effects
    # smokes because distro Qt 6.4 has no QtQuick.Effects module.
    add_executable(youtube-telemetry-smoke src/tests/youtube-telemetry-smoke-test.cpp)
    set_target_properties(youtube-telemetry-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(youtube-telemetry-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
    target_compile_definitions(youtube-telemetry-smoke PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME youtube-telemetry-smoke COMMAND youtube-telemetry-smoke)
endif()

# ── CPack ──────────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "240-MP")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "240-MP Media Player")
set(CPACK_PACKAGE_VENDOR "240-MP")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "240-MP")
    set(CPACK_DMG_FORMAT "UDZO")
else()
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
endif()

#   cmake -B build -DBUILD_TESTS=ON
#   cmake --build build --target mpv-basic-options-tests && ./build/mpv-basic-options-tests
option(BUILD_TESTS "Build the unit test suite" OFF)
if(BUILD_TESTS)

    enable_testing()
    # MpvBasicOptions policy-table tests: pure file-in/args-out checks against
    # a temp dataRoot, no process spawning.
    add_executable(mpv-basic-options-tests
        src/util/tests/MpvBasicOptionsTest.cpp
        src/util/MpvBasicOptions.cpp)
    set_target_properties(mpv-basic-options-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-basic-options-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(mpv-basic-options-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-basic-options COMMAND mpv-basic-options-tests)

    # T2 (issue #111): per-instance mpv temp paths — pure helper tests.
    # No process spawning; exercise the sun_path bound and uniqueness logic.
    add_executable(mpv-instance-paths-tests
        src/util/tests/MpvInstancePathsTest.cpp)
    set_target_properties(mpv-instance-paths-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-instance-paths-tests PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(mpv-instance-paths-tests PRIVATE src)
    if(NOT MSVC)
        target_compile_options(mpv-instance-paths-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-instance-paths COMMAND mpv-instance-paths-tests)

    # Built-in palette consistency tests: themes table (Main.qml) vs the Settings
    # picker list (views/Settings.qml) — drift guard for adding color schemes.
    add_executable(palette-tests
        src/util/tests/ColorSchemesTest.cpp)
    set_target_properties(palette-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(palette-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(palette-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME palette-consistency COMMAND palette-tests)

    # YouTube direct-URL cache: pure helpers (expire-parameter parse, freshness
    # margin, prune cap). No processes, no network (issue #28 plan A).
    add_executable(yt-url-cache-tests
        src/util/tests/YtUrlFreshTest.cpp)
    set_target_properties(yt-url-cache-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-url-cache-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-url-cache-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-url-cache COMMAND yt-url-cache-tests)

    # Warm-403 reactive cure (2026-11-07 night): eviction semantics the Player
    # retry leans on — exact-key removal, format isolation, harmless miss,
    # no resurrection through prune. Same pure helpers, no processes/network.
    add_executable(yt-url-invalidate-tests
        src/util/tests/YtUrlCacheInvalidateTest.cpp)
    set_target_properties(yt-url-invalidate-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-url-invalidate-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-url-invalidate-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-url-invalidate COMMAND yt-url-invalidate-tests)

    # Durable negative cache for permanent YouTube resolve failures. Exercises
    # classification, the 24 h TTL and the real on-disk persistence helpers.
    add_executable(yt-negative-cache-tests
        src/util/tests/YtNegativeCacheTest.cpp)
    set_target_properties(yt-negative-cache-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-negative-cache-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-negative-cache-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-negative-cache COMMAND yt-negative-cache-tests)

    # Backend wiring gate for the same cache: list warm, focus warm and press
    # must not reach either resolver, including after a backend restart.
    add_executable(yt-negative-cache-backend-tests
        src/modules/youtube/tests/YouTubeNegativeCacheIntegrationTest.cpp
        src/modules/youtube/YouTubeBackend.cpp
        src/modules/youtube/YtInnertubeResolver.cpp)
    set_target_properties(yt-negative-cache-backend-tests PROPERTIES
        AUTOMOC ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_include_directories(yt-negative-cache-backend-tests PRIVATE src)
    target_link_libraries(yt-negative-cache-backend-tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test innertube-qt)
    if(NOT MSVC)
        target_compile_options(yt-negative-cache-backend-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-negative-cache-backend COMMAND yt-negative-cache-backend-tests)

    # Persistence gate for the bounded ID-keyed state (MEM-01 follow-up):
    # oversized/legacy/corrupt files must load straight into the caps, self-heal
    # into bounded MRU-first writes, and the debounced save must flush on exit.
    # Seeds files and constructs the real backend — no network, no proxy.
    add_executable(yt-sponsor-persistence-tests
        src/modules/youtube/tests/YouTubeSponsorPersistenceTest.cpp
        src/modules/youtube/YouTubeBackend.cpp
        src/modules/youtube/YtInnertubeResolver.cpp)
    set_target_properties(yt-sponsor-persistence-tests PROPERTIES
        AUTOMOC ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_include_directories(yt-sponsor-persistence-tests PRIVATE src)
    target_link_libraries(yt-sponsor-persistence-tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test innertube-qt)
    if(NOT MSVC)
        target_compile_options(yt-sponsor-persistence-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-sponsor-persistence COMMAND yt-sponsor-persistence-tests)

    # OAuth credential-expiry gate (issue #28): pure clock math for refresh-
    # before-expiry semantics. No processes, no network.
    add_executable(yt-auth-gate-tests
        src/util/tests/YtAuthGateTest.cpp)
    set_target_properties(yt-auth-gate-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-auth-gate-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-auth-gate-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-auth-gate COMMAND yt-auth-gate-tests)

    # OAuth refresh replay state (youtube torture 2026-11-18): a marker is
    # set only while a request is deferred; live requests clear their own.
    # Pure state machine, no processes/network.
    add_executable(yt-pending-replay-tests
        src/util/tests/YtPendingReplayTest.cpp)
    set_target_properties(yt-pending-replay-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-pending-replay-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-pending-replay-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-pending-replay COMMAND yt-pending-replay-tests)

    # InnerTube browse-row parser (issue #28 plan D): pure fixture walk —
    # renderer extraction, text-shape handling, caps, continuation cursors.
    add_executable(yt-browse-parse-tests
        src/util/tests/YtBrowseRowsTest.cpp)
    set_target_properties(yt-browse-parse-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-browse-parse-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-browse-parse-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-browse-parse COMMAND yt-browse-parse-tests)

    # SponsorBlock -> mpv EDL pure logic (issue #28 plan E): videoUID collision
    # filter, dead-range guards, merge, %len% escape, virtual-timeline gaps.
    add_executable(yt-sponsor-edl-tests
        src/util/tests/YtSponsorEdlTest.cpp)
    set_target_properties(yt-sponsor-edl-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-sponsor-edl-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-sponsor-edl-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-sponsor-edl COMMAND yt-sponsor-edl-tests)

    # SponsorBlock fetch gate (latency follow-up): transient failures are
    # negative-cached for a short TTL so a dead sponsor.ajay.app costs one
    # request per video per window, a confirmed-empty window is a permanent
    # hit, and the suppression map is pruned. Pure clock math.
    add_executable(yt-sponsor-ttl-tests
        src/util/tests/YtSponsorTtlTest.cpp)
    set_target_properties(yt-sponsor-ttl-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-sponsor-ttl-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-sponsor-ttl-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-sponsor-ttl COMMAND yt-sponsor-ttl-tests)

    # Bounded eviction: hard LRU caps on the ID-keyed SponsorBlock and
    # known-id state. Plateau, hot-entry survival, segment/timestamp co-eviction
    # and clean refetch of evicted ids. Pure container math.
    add_executable(yt-sponsor-lru-tests
        src/util/tests/YtSponsorLruTest.cpp)
    set_target_properties(yt-sponsor-lru-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-sponsor-lru-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-sponsor-lru-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-sponsor-lru COMMAND yt-sponsor-lru-tests)

    # Innertube stream selection (overnight torture t_7ffb1fd1): pure fixture
    # tests for the cap-enforcement, H.264 preference, itag-140 audio pick,
    # muxed fallback, and expiry derivation. No network, no Qt event loop,
    # no vendored library.
    add_executable(yt-stream-select-tests
        src/util/tests/YtStreamSelectTest.cpp)
    set_target_properties(yt-stream-select-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-stream-select-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-stream-select-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-stream-select COMMAND yt-stream-select-tests)

    # Live HLS master shaping (#128): pure fixture tests for the variant cap,
    # audio-rendition pruning, URI absolutization and the lowest-known
    # fallbacks. No network, no Qt event loop, no vendored library.
    add_executable(yt-hls-select-tests
        src/util/tests/YtHlsSelectTest.cpp)
    set_target_properties(yt-hls-select-tests PROPERTIES AUTOMOC ON)
    target_include_directories(yt-hls-select-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(yt-hls-select-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-hls-select-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-hls-select COMMAND yt-hls-select-tests)

    # Subscription-union dedup (#128): videoId repeat + live double-post
    # collapse, with the far-apart same-title and missing-metadata safety arms.
    add_executable(yt-feed-dedup-tests
        src/util/tests/YtFeedDedupTest.cpp)
    set_target_properties(yt-feed-dedup-tests PROPERTIES AUTOMOC ON)
    target_include_directories(yt-feed-dedup-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(yt-feed-dedup-tests PRIVATE Qt6::Core Qt6::Test)
    if(NOT MSVC)
        target_compile_options(yt-feed-dedup-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-feed-dedup COMMAND yt-feed-dedup-tests)

    # Lab gate probe (NOT a ctest test — it needs the network and mpv).
    # Resolves through the real YtInnertubeResolver and requires mpv to decode
    # BOTH streams. A curl Range probe is not a playability proof: IOS URLs
    # answer bounded ranges but 403 the open-ended range ffmpeg sends.
    #   ./build-test/yt-resolver-probe <videoId>
    add_executable(yt-resolver-probe
        src/modules/youtube/tests/YtResolverProbe.cpp
        src/modules/youtube/YtInnertubeResolver.cpp)
    set_target_properties(yt-resolver-probe PROPERTIES AUTOMOC ON CXX_STANDARD 20)
    target_link_libraries(yt-resolver-probe PRIVATE
        Qt6::Core Qt6::Network innertube-qt)
    target_include_directories(yt-resolver-probe PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/innertube-qt/src
        ${CMAKE_SOURCE_DIR}/src/util)

# ROB-07 one-terminal-result contract (YtInnertubeResolver.h): exactly one
    # finished() per accepted resolve(), and an attempt abandoned by the timeout
    # must never emit again nor chain a Player request. Two-sided regression:
    # delayed-abandoned (visitorData replies AFTER the 4 s timeout), near-deadline
    # (replies just BEFORE it), fast-healthy control (over-suppression guard).
    # Drives the REAL resolver + vendored library against a loopback CONNECT+TLS
    # fixture proxy — no live network, no mpv, no display. Delays are fixture-side
    # sleeps, configurable via --visitor-delay-ms/--observe-ms on the case script
    # or YT_CONTRACT_VISITOR_DELAY_MS/YT_CONTRACT_OBSERVE_MS. Needs python3 and
    # the openssl CLI (ephemeral self-signed fixture cert, generated per run).
    # On the unfixed snapshot delayed-abandoned FAILS (phantom ok=1 after the
    # timeout); the other two cases pass. See src/modules/youtube/tests/README.md.
    #   cmake -B build-test -DBUILD_TESTS=ON
    #   cmake --build build-test --target yt-resolver-contract-probe
    #   ctest --test-dir build-test -R yt-resolver-contract --output-on-failure
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(yt-resolver-contract-probe
        src/modules/youtube/tests/YtResolverContractProbe.cpp
        src/modules/youtube/YtInnertubeResolver.cpp)
    set_target_properties(yt-resolver-contract-probe PROPERTIES AUTOMOC ON CXX_STANDARD 20)
    target_link_libraries(yt-resolver-contract-probe PRIVATE
        Qt6::Core Qt6::Network innertube-qt)
    target_include_directories(yt-resolver-contract-probe PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/innertube-qt/src
        ${CMAKE_SOURCE_DIR}/src/util)
    if(NOT MSVC)
        target_compile_options(yt-resolver-contract-probe PRIVATE -Wall -Wextra)
    endif()
    foreach(contract_case delayed-abandoned near-deadline fast-healthy
                         live-waiting live-hls live-hls-with-pair
                         post-live-hls premiered-vod-pair)
        add_test(NAME yt-resolver-contract-${contract_case}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/youtube/tests/run_contract_case.py
                --case ${contract_case}
                --probe $<TARGET_FILE:yt-resolver-contract-probe>
                --fixture ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/youtube/tests/fixture_proxy.py)
        set_tests_properties(yt-resolver-contract-${contract_case} PROPERTIES
            TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    # MEM-02 visitorData size-cap regression (two-sided, loopback TLS fixture,
    # no live network): healthy controls (small page, real 450-char token,
    # chunk-boundary straddles) must resolve ok=1 with the EXACT token chained
    # into the Player request; oversize pages must fail cleanly with
    # reason=visitordata-size-cap into the backend's unconditional yt-dlp
    # ladder, with bounded bytes-consumed/HWM accounting. On the pre-fix
    # snapshot cap-plus-one / oversize-* / real-size-early-exit FAIL; the
    # healthy controls pass on both sides. See src/modules/youtube/tests/README.md.
    #   cmake -B build-test -DBUILD_TESTS=ON
    #   cmake --build build-test --target yt-visitordata-cap-probe
    #   ctest --test-dir build-test -R yt-visitordata-cap --output-on-failure
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(yt-visitordata-cap-probe
        src/modules/youtube/tests/YtVdCapProbe.cpp
        src/modules/youtube/YtInnertubeResolver.cpp)
    set_target_properties(yt-visitordata-cap-probe PROPERTIES AUTOMOC ON CXX_STANDARD 20)
    target_link_libraries(yt-visitordata-cap-probe PRIVATE
        Qt6::Core Qt6::Network innertube-qt)
    target_include_directories(yt-visitordata-cap-probe PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/innertube-qt/src
        ${CMAKE_SOURCE_DIR}/src/util)
    if(NOT MSVC)
        target_compile_options(yt-visitordata-cap-probe PRIVATE -Wall -Wextra)
    endif()
    foreach(vd_cap_case
            healthy-control realistic-token straddle-key-64k straddle-value-64k
            straddle-value-256k exact-cap cap-plus-one oversize-no-token
            oversize-token no-token-under-cap real-size-early-exit)
        add_test(NAME yt-visitordata-cap-${vd_cap_case}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/youtube/tests/run_vd_cap_case.py
                --case ${vd_cap_case}
                --probe $<TARGET_FILE:yt-visitordata-cap-probe>
                --fixture ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/youtube/tests/vd_cap_fixture_proxy.py)
        set_tests_properties(yt-visitordata-cap-${vd_cap_case} PROPERTIES
            TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    # DisplayHandoff signal contract (issue #75, PR #77): displayTaken() fires
    # exactly once per completed hand-off and only AFTER the CRTC state is
    # captured, displayReturned() once per return, and a refused hand-off stays
    # silent. Needs no DRM device and no VT: the capture step is injected through
    # setCrtcCaptureHookForTest() and the privileged steps fail cleanly when
    # unprivileged, so this runs in the lab and on CI alike.
    add_executable(display-handoff-tests
        src/util/tests/DisplayHandoffSignalTest.cpp
        src/util/DisplayHandoff.cpp)
    set_target_properties(display-handoff-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(display-handoff-tests PRIVATE Qt6::Core Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    if(NOT APPLE)
        target_include_directories(display-handoff-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    if(NOT MSVC)
        target_compile_options(display-handoff-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME display-handoff-signal COMMAND display-handoff-tests)

    # VT-01 regression gate: armed-connection + no-premature-restore contract.
    # Linux-only (VT ioctls / linux/vt.h); needs libdrm for the interposed capture
    # path. One executable, one ctest case per arm; RUN_SERIAL because the arms
    # wait on the real 800 ms restore timer and CI runs ctest -j4.
    if(UNIX AND NOT APPLE)
        add_executable(display-handoff-connection-gate
            src/util/tests/DisplayHandoffConnectionGate.cpp
            src/util/DisplayHandoff.cpp)
        set_target_properties(display-handoff-connection-gate PROPERTIES AUTOMOC ON)
        target_include_directories(display-handoff-connection-gate PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/util ${Libdrm_INCLUDE_DIRS})
        target_link_libraries(display-handoff-connection-gate PRIVATE
            Qt6::Core Qt6::Gui Qt6::Quick ${Libdrm_LIBRARIES})
        target_compile_options(display-handoff-connection-gate PRIVATE -Wall -Wextra)
        foreach(gate_arm
            sanity timer-wins frame-wins stale-frame-keepwindow inflight-hide
            inflight-after-accumulation benign-after-return idempotence
            ordering-hide ordering-keepwindow)
            add_test(NAME display-handoff-gate-${gate_arm}
                COMMAND display-handoff-connection-gate ${gate_arm})
            set_tests_properties(display-handoff-gate-${gate_arm} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DISPLAY=;WAYLAND_DISPLAY="
                TIMEOUT 180
                RUN_SERIAL TRUE)
        endforeach()
    endif()

    # R-2 (issue #109): YouTube launch-guard routing. Loads the real Player.qml
    # with stub backends and asserts a Back in each launch phase routes to
    # sendKey (post-launch) vs goBack (pre-launch) — the launchDone flag, not the
    # first-position playbackStarted flag, gates the quit intent.
    add_executable(youtube-quit-intent-smoke src/tests/youtube-quit-intent-smoke-test.cpp)
    set_target_properties(youtube-quit-intent-smoke PROPERTIES AUTOMOC ON)
    target_link_libraries(youtube-quit-intent-smoke PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
    target_compile_definitions(youtube-quit-intent-smoke PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME youtube-quit-intent-smoke COMMAND youtube-quit-intent-smoke)

    # R-2 (issue #109): quit-intent hybrid against the REAL MpvController + mpv.
    # SPEC-1 pre-connect invariant x100 (ESC/BS/STOP), SPEC-2 post-connect lua
    # preservation, SPEC-4 loop-on bounded exit. Real AppCore (config-driven
    # mpv_video_args/screensaver_timeout); QSKIPs when mpv/ffmpeg are absent so
    # CI lanes without a media stack stay green.
    add_executable(mpv-quit-intent-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/FontconfigOverride.cpp
        src/tests/MpvQuitIntentTest.cpp)
    set_target_properties(mpv-quit-intent-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-quit-intent-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(mpv-quit-intent-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(mpv-quit-intent-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    target_compile_definitions(mpv-quit-intent-tests PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mpv-quit-intent-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-quit-intent COMMAND mpv-quit-intent-tests)
    # QTEST_MAIN constructs a QGuiApplication before any main-body code runs, so
    # the offscreen platform must come from the environment (the QML smokes can
    # qputenv it in main; this one cannot). Headless CI lanes have no display.
    set_tests_properties(mpv-quit-intent PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # #124 hold-Right ramp consistency: REAL MpvController + REAL mpv + the REAL
    # YouTube Player.qml driven through the RAW key path (QKeyEvents posted at
    # the window — the remote's eventFilter return-false downstream). Two-sided:
    # MP240_HOLD_EXPECT=fixed asserts the deterministic ramp; =defective asserts
    # the known-bad pre-fix shapes (absent/mislabeled/variable-rate autorepeat).
    # Loads Player.qml like youtube-seek-smoke, so the arm64 distro-Qt lane
    # excludes it the same way; QSKIPs without mpv/ffmpeg. RUN_SERIAL: the dwell
    # bands are wall-clock and must not race parallel ctest load.
    add_executable(mpv-hold-repeat-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/FontconfigOverride.cpp
        src/tests/MpvHoldRepeatTest.cpp)
    set_target_properties(mpv-hold-repeat-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-hold-repeat-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(mpv-hold-repeat-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(mpv-hold-repeat-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    target_compile_definitions(mpv-hold-repeat-tests PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mpv-hold-repeat-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-hold-repeat COMMAND mpv-hold-repeat-tests)
    set_tests_properties(mpv-hold-repeat PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RUN_SERIAL TRUE)

    # R2 follow-up (issue #110): FailedToStart after a successful locate() must
    # emit a synthetic playbackEnded("failed") instead of leaving the Player
    # view waiting forever. Forces the spawn failure through the REAL locator
    # (private PATH dir holding an executable-but-invalid "mpv" -> ENOEXEC).
    # The core cases need no mpv installed, so every CI lane runs the gate for
    # real; only the relaunch-after-failure case QSKIPs without mpv/ffmpeg.
    add_executable(mpv-failedtostart-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/FontconfigOverride.cpp
        src/tests/MpvFailedToStartTest.cpp)
    set_target_properties(mpv-failedtostart-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-failedtostart-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(mpv-failedtostart-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(mpv-failedtostart-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    target_compile_definitions(mpv-failedtostart-tests PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mpv-failedtostart-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-failedtostart COMMAND mpv-failedtostart-tests)
    # Same QTEST_MAIN/offscreen constraint as mpv-quit-intent above. Isolate the
    # IPC socket into a per-build TMPDIR so parallel tests cannot race on it.
    # Keep the path short: mpv's Unix socket is limited by sockaddr_un.sun_path
    # (108 bytes on Linux), which a nested worktree build dir can exceed.
    set(_failedtostart_tmp_base "$ENV{TMPDIR}")
    if(NOT _failedtostart_tmp_base)
        set(_failedtostart_tmp_base "/tmp")
    endif()
    string(MD5 _failedtostart_tmp_hash "${CMAKE_CURRENT_BINARY_DIR}")
    string(SUBSTRING "${_failedtostart_tmp_hash}" 0 8 _failedtostart_tmp_hash)
    set(_failedtostart_tmp "${_failedtostart_tmp_base}/240mp-ft-${_failedtostart_tmp_hash}")
    file(MAKE_DIRECTORY "${_failedtostart_tmp}")
    set_tests_properties(mpv-failedtostart PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TMPDIR=${_failedtostart_tmp}")

    # #145 CRT repro: real mpv against the shaped local HLS master. The mpv
    # resolver contract proved classification but never launched mpv, so the
    # whitelist failure reached the owner's CRT. This gate reproduces it with
    # a loopback HLS origin and the production arg helper. Needs real mpv +
    # ffmpeg; QSKIPs on lanes missing either.
    add_executable(yt-live-hls-local-tests
        src/AppCore.cpp
        # CFG-01: AppCore now calls cfgfile::load/save/reset; link the impl.
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/FontconfigOverride.cpp
        src/tests/YtLiveHlsLocalPlaylistTest.cpp)
    set_target_properties(yt-live-hls-local-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(yt-live-hls-local-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(yt-live-hls-local-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(yt-live-hls-local-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    target_compile_definitions(yt-live-hls-local-tests PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(yt-live-hls-local-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME yt-live-hls-local COMMAND yt-live-hls-local-tests)
    string(MD5 _lhl_tmp_hash "${CMAKE_CURRENT_BINARY_DIR}")
    string(SUBSTRING "${_lhl_tmp_hash}" 0 8 _lhl_tmp_hash)
    set(_lhl_tmp "${_failedtostart_tmp_base}/240mp-lhl-${_lhl_tmp_hash}")
    file(MAKE_DIRECTORY "${_lhl_tmp}")
    set_tests_properties(yt-live-hls-local PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TMPDIR=${_lhl_tmp}")

    # T2 (issue #111): real MpvController per-instance socket contract.
    # No mpv spawning — constructs the controller and checks socketPath()/logFilePath().
    add_executable(mpv-instance-socket-tests
        src/AppCore.cpp
        src/util/ConfigFile.cpp
        src/player/MpvController.cpp
        src/player/SeekHoldPolicy.cpp
        src/util/DisplayHandoff.cpp
        src/util/MpvBasicOptions.cpp
        src/util/MpvLocator.cpp
        src/util/FontconfigOverride.cpp
        src/tests/MpvInstanceSocketTest.cpp)
    set_target_properties(mpv-instance-socket-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(mpv-instance-socket-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Network Qt6::Test
        $<$<NOT:$<BOOL:${APPLE}>>:${Libdrm_LIBRARIES}>)
    target_include_directories(mpv-instance-socket-tests PRIVATE src)
    if(NOT APPLE)
        target_include_directories(mpv-instance-socket-tests PRIVATE ${Libdrm_INCLUDE_DIRS})
    endif()
    target_compile_definitions(mpv-instance-socket-tests PRIVATE
        MP240_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mpv-instance-socket-tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mpv-instance-socket COMMAND mpv-instance-socket-tests)
    set_tests_properties(mpv-instance-socket PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

endif()

include(CPack)
