#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QDir>
#include <QStandardPaths>
#include <QCursor>
#include <QDebug>
#include <QWindow>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <locale.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <QThread>
#include <QRegularExpression>

#include <memory>

#include "AppCore.h"
#include "modules/local_files/LocalFilesBackend.h"
#include "modules/plex/PlexBackend.h"
#include "modules/jellyfin/JellyfinBackend.h"
#include "modules/emby/EmbyBackend.h"
#include "modules/ambient_mode/AmbientModeBackend.h"
#include "modules/nfc_reader/NfcReaderBackend.h"
#include "modules/youtube/YouTubeBackend.h"
#include "modules/weather/WeatherBackend.h"
#include "modules/scripts/ScriptsBackend.h"
#include "modules/retroarch/RetroArchBackend.h"
#include "modules/virtual_channels/VirtualChannelsBackend.h"
#include "player/MpvController.h"
#include "input/InputManager.h"
#include "input/IdleTracker.h"
#include "audio/VolumeManager.h"
#include "update/UpdateManager.h"
#include "util/ExecPath.h"
#include "util/DisplayHandoff.h"
#include "util/HandoffWindowPolicy.h"
#ifdef Q_OS_MAC
#include "util/MacosUtils.h"
#endif
#ifdef Q_OS_LINUX
#include <fontconfig/fontconfig.h>
#include <cstdlib>
#endif

static QString resolveAppRoot() {
    QString envRoot = qEnvironmentVariable("APP_ROOT");
    if (!envRoot.isEmpty())
        return QDir(envRoot).canonicalPath();

    QString appDir = QCoreApplication::applicationDirPath();

    if (QCoreApplication::applicationFilePath().contains(".app/Contents/MacOS/"))
        return QDir(appDir + "/../Resources").canonicalPath();

    QDir fhsData(appDir + "/../share/240mp");
    if (fhsData.exists())
        return fhsData.canonicalPath();

    return QDir(appDir + "/..").canonicalPath();
}

static QString resolveDataRoot() {
    QString envRoot = qEnvironmentVariable("DATA_ROOT");
    if (!envRoot.isEmpty())
        return QDir(envRoot).canonicalPath();

    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(path);
    return path;
}

// Terminating signals must unwind normally rather than killing the process where
// it stands. Without this, `systemctl stop`, `pkill`, or a shutdown can leave mpv
// or another process running and with the scripts module (takeover specifically), it
// could also leave a headless Pi on a blank VT with DRM master dropped, because
// ~DisplayHandoff never got the chance to put the display back.
//
// Async-signal-safe: only sets a flag. A 100 ms timer in main() polls it and calls
// quit() from the event loop, where destructors run properly.
static volatile std::sig_atomic_t g_termRequested = 0;
extern "C" void mp240HandleTerm(int) { g_termRequested = 1; }


// Anything a server token is carried in can end up in a log without anyone
// meaning to put it there: Qt prints the URL of an image that fails to load,
// and mpv prints the URL it could not open. Both carry the token that fetched
// the artwork or the stream, and qWarning survives a release build. So every
// message goes past here first, and the token never reaches the journal.
//
// The message is handed on to the handler that was installed before this one,
// rather than logged again from in here: logging from inside a log handler
// comes straight back to it.
static QtMessageHandler g_priorMessageHandler = nullptr;

static void redactingMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                                    const QString &message) {
    static const QRegularExpression tokenInUrl(
        QStringLiteral("((?:X-Plex-Token|api_key|ApiKey|X-Emby-Token|X-MediaBrowser-Token|"
                       "accessToken|X-Plex-Client-Identifier)=)[^&\\s\"'<>]+"),
        QRegularExpression::CaseInsensitiveOption);

    if (!message.contains(QLatin1Char('='))) {
        if (g_priorMessageHandler) g_priorMessageHandler(type, ctx, message);
        return;
    }

    QString clean = message;
    clean.replace(tokenInUrl, QStringLiteral("\\1<redacted>"));
    if (g_priorMessageHandler) g_priorMessageHandler(type, ctx, clean);
}

#ifdef Q_OS_LINUX
// Main.qml's FontLoaders register the bundled fonts through
// QFontDatabase::addApplicationFont, which makes fontconfig build its global
// config on first use. fontconfig keeps that config for the process lifetime
// and Qt never calls FcFini(), so LSan reports the config's parse allocations
// as leaked at exit (one-time, ~320 B in 3 blocks — not per-transition growth).
// Release them on the way out: registered before any font is loaded, this
// handler runs after main()'s stack objects (engine, windows, app) and Qt's
// font database are destroyed, and before the sanitizer's leak check.
static void releaseFontconfigGlobals() { FcFini(); }
#endif

int main(int argc, char *argv[]) {
    g_priorMessageHandler = qInstallMessageHandler(redactingMessageHandler);
    QGuiApplication app(argc, argv);
    app.setApplicationName("240-MP");
    app.setApplicationVersion(QStringLiteral(APP_VERSION));

    // Hide cursor — 240-MP is keyboard/gamepad-only so the cursor serves no
    // purpose. Hidden on all of Linux: headless EGLFS and desktop compositors
    // (Steam Deck / RPi desktop) alike, since the app runs fullscreen kiosk-style.
#ifdef Q_OS_LINUX
    QGuiApplication::setOverrideCursor(Qt::BlankCursor);
#endif
#ifdef Q_OS_MAC
    QGuiApplication::setOverrideCursor(Qt::BlankCursor);
    hideMacOSMenuBar();
#endif

    setlocale(LC_NUMERIC, "C");

#ifdef Q_OS_LINUX
    std::atexit(releaseFontconfigGlobals);
#endif

    std::signal(SIGTERM, mp240HandleTerm);
    std::signal(SIGINT,  mp240HandleTerm);
    std::signal(SIGHUP,  mp240HandleTerm);
    QTimer termPoll;
    termPoll.setInterval(100);
    QObject::connect(&termPoll, &QTimer::timeout, &app, [&app]() {
        if (g_termRequested) {
            qInfo("[main] Termination signal received — shutting down cleanly");
            app.quit();
        }
    });
    termPoll.start();

    // Once, before anything looks for or spawns mpv: the locator is a pure
    // query and deliberately does not touch the environment itself.
    execpath::primeSystemPath();

    const QString appRoot  = resolveAppRoot();
    const QString dataRoot = resolveDataRoot();
    qDebug("[main] appRoot  = %s", qPrintable(appRoot));
    qDebug("[main] dataRoot = %s", qPrintable(dataRoot));

    // Log every attached display so a user can discover which index is their
    // target (e.g. a CRT) and set the app-level "display_index" in config.json
    // accordingly. Name is the localized display name on macOS, the RandR
    // output name on X11, the wl_output name on Wayland, and the single DRM
    // output on EGLFS.
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QRect g = screens.at(i)->geometry();
        qInfo("[main] display index %d: \"%s\" %dx%d at (%d,%d)",
              i, qPrintable(screens.at(i)->name()),
              g.width(), g.height(), g.x(), g.y());
    }

    AppCore             appCore(appRoot, dataRoot);

    // Which physical display the UI launches on. App-level "display_index"
    // (0 = primary screen, the previous hardcoded behaviour). Lets the UI open
    // on a secondary display without making it the OS primary. Applied on
    // macOS and desktop Linux (xcb/wayland); on single-display platforms
    // (Pi EGLFS, Steam Deck gaming mode under gamescope) the clamp below
    // resolves to 0 and everything stays a no-op.
    // On macOS, Qt's screen list and AppKit's NSScreen.screens share ordering,
    // so this index is valid for both the QML geometry below and the native
    // forceWindowFullScreenOnScreen() call after load.
    int displayIndex = appCore.get_setting(QString(), "display_index").toInt();
    if (displayIndex < 0 || displayIndex >= screens.size()) {
        if (displayIndex != 0)
            qWarning("[main] display_index %d is out of range (%lld displays attached) — falling back to display 0",
                     displayIndex, (long long)screens.size());
        displayIndex = 0;
    }
    QScreen *targetScreen = screens.value(displayIndex, QGuiApplication::primaryScreen());
    const QRect screenGeo = targetScreen ? targetScreen->geometry() : QRect(0, 0, 1920, 1080);
    qInfo("[main] UI target display index %d -> %dx%d at (%d,%d)",
          displayIndex, screenGeo.width(), screenGeo.height(), screenGeo.x(), screenGeo.y());
    // Auto Fit fit-target source: mpv's DRM VOs (the Pi path) have no windowing
    // backend and can't report display size over IPC; the app's own startup
    // enumeration IS the KMS plane vo=drm draws to. Hand it to AppCore once.
    appCore.setDisplayGeometry(screenGeo.width(), screenGeo.height());

    // Module gating: the per-module "enabled" flag in config.json (manifest
    // default when unset) decides what is built, read once at startup, so a
    // change in Settings takes effect on the next launch. A backend is built
    // when enabled; it reaches QML only when enabled. virtual_channels takes
    // plex/jellyfin/emby by reference, so those are built (not exposed) whenever
    // virtual_channels is on. Owned by unique_ptr, declared in the old stack
    // order, so destruction still runs in reverse declaration order.
    auto enabled = [&appCore](const QString &id) { return appCore.is_module_enabled(id); };
    const bool vcOn       = enabled(QStringLiteral("com.240mp.virtual_channels"));
    const bool buildPlex     = enabled(QStringLiteral("com.240mp.plex"))     || vcOn;
    const bool buildJellyfin = enabled(QStringLiteral("com.240mp.jellyfin")) || vcOn;
    const bool buildEmby     = enabled(QStringLiteral("com.240mp.emby"))     || vcOn;
    std::unique_ptr<LocalFilesBackend>     localFiles;
    std::unique_ptr<PlexBackend>           plexBackend;
    std::unique_ptr<JellyfinBackend>       jellyfinBackend;
    std::unique_ptr<EmbyBackend>           embyBackend;
    std::unique_ptr<AmbientModeBackend>    ambientMode;
    std::unique_ptr<NfcReaderBackend>      nfcReader;
    std::unique_ptr<YouTubeBackend>        youtubeBackend;
    std::unique_ptr<WeatherBackend>        weatherBackend;

    if (enabled(QStringLiteral("com.240mp.local_files")))
        localFiles = std::make_unique<LocalFilesBackend>(appRoot, dataRoot);
    if (buildPlex)
        plexBackend = std::make_unique<PlexBackend>(appRoot, dataRoot);
    if (buildJellyfin)
        jellyfinBackend = std::make_unique<JellyfinBackend>(appRoot, dataRoot);
    if (buildEmby)
        embyBackend = std::make_unique<EmbyBackend>(appRoot, dataRoot);
    if (enabled(QStringLiteral("com.240mp.ambient_mode")))
        ambientMode = std::make_unique<AmbientModeBackend>(dataRoot);
    if (enabled(QStringLiteral("com.240mp.nfc_reader")))
        nfcReader = std::make_unique<NfcReaderBackend>(appRoot, dataRoot, &appCore);
    if (enabled(QStringLiteral("com.240mp.youtube")))
        youtubeBackend = std::make_unique<YouTubeBackend>(appRoot, dataRoot);
    if (enabled(QStringLiteral("com.240mp.weather")))
        weatherBackend = std::make_unique<WeatherBackend>(appRoot, dataRoot);

    // DisplayHandoff is a core singleton: always live. Declared before the
    // backends below that hold a pointer to it, so it outlives them.
    DisplayHandoff      displayHandoff;

    // RetroArch is also the InputManager game observer; scripts and
    // virtual_channels are built alongside it as well (kept from the draft
    // gate; conservative, since the observer is nullable anyway).
    std::unique_ptr<RetroArchBackend>      retroArch;
    std::unique_ptr<ScriptsBackend>        scriptsBackend;
    std::unique_ptr<VirtualChannelsBackend> virtualChannels;
    if (enabled(QStringLiteral("com.240mp.retroarch"))
        || enabled(QStringLiteral("com.240mp.scripts"))
        || enabled(QStringLiteral("com.240mp.virtual_channels")))
        retroArch = std::make_unique<RetroArchBackend>(appRoot, dataRoot, &displayHandoff);
    if (enabled(QStringLiteral("com.240mp.scripts")))
        scriptsBackend = std::make_unique<ScriptsBackend>(appRoot, dataRoot, &displayHandoff);
    if (enabled(QStringLiteral("com.240mp.virtual_channels")))
        virtualChannels = std::make_unique<VirtualChannelsBackend>(
            appRoot, dataRoot,
            plexBackend.get(), jellyfinBackend.get(), embyBackend.get());

    MpvController       mpvController(appRoot, dataRoot, &appCore, &displayHandoff);
    InputManager        inputManager(dataRoot, &appCore);
    inputManager.setDisplayHandoff(&displayHandoff);
    // Nullable observer: no RetroArch backend means no game-window tracking.
    inputManager.setGameObserver(retroArch.get());
    VolumeManager       volumeManager(dataRoot);
    inputManager.setVolumeManager(&volumeManager);
    IdleTracker         idleTracker(60);   // disabled until Main.qml applies the saved setting
    UpdateManager       updateManager(appRoot, dataRoot);


    // Playback follows the UI's display: mpv gets a --fs-screen* arg derived
    // from this on macOS / desktop Linux (no-op at index 0 and on headless).
    mpvController.setTargetDisplay(displayIndex, targetScreen ? targetScreen->name() : QString());

    // When the Qt window is inactive (fullscreen mpv has OS focus on macOS),
    // gamepad actions bypass QML and drive mpv directly over IPC.
    QObject::connect(&inputManager, &InputManager::mpvKeyRequested,
                     &mpvController, &MpvController::sendKey);
    QObject::connect(&inputManager, &InputManager::mpvKeyReleased,
                     &mpvController, &MpvController::sendKeyUp);

    // The child-owned renderer for the same number: while mpv owns the screen the
    // app's QML bar cannot draw, so the level is pushed to the mpv lua OSD. With
    // no child running the call is dropped and Main.qml's bar does the drawing.
    QObject::connect(&volumeManager, &VolumeManager::volumeChanged,
                     &mpvController, [&mpvController](int level, bool muted) {
                         mpvController.showVolumeOsd(level, muted);
                     });

    // Each module backend is wired in one call: stored for action routing, exposed to QML
    // introspection. The module ID lives in exactly one place per module.
    // LIFECYCLE-01: the engine is declared AFTER the stack singletons so C++
    // unwinding destroys it FIRST. QML Component.onDestruction handlers then
    // run while every context property is still a live object. Before this the
    // engine was declared above AppCore and destructed last, so handlers saw
    // null wrappers and their exit cleanup threw TypeError.
    QQmlApplicationEngine engine;
    QQmlContext *ctx = engine.rootContext();
    // Built-but-disabled backends (dependencies) register as null, so QML never
    // sees them and their background work stays off.
    auto exposed = [&enabled](const char *id, QObject *obj) -> QObject * {
        return enabled(QString::fromLatin1(id)) ? obj : nullptr;
    };
    appCore.registerModule("com.240mp.local_files",  "localFilesBackend",  exposed("com.240mp.local_files", localFiles.get()), ctx);
    appCore.registerModule("com.240mp.plex",         "plexBackend",        exposed("com.240mp.plex", plexBackend.get()), ctx);
    appCore.registerModule("com.240mp.jellyfin",     "jellyfinBackend",    exposed("com.240mp.jellyfin", jellyfinBackend.get()), ctx);
    appCore.registerModule("com.240mp.emby",         "embyBackend",        exposed("com.240mp.emby", embyBackend.get()), ctx);
    appCore.registerModule("com.240mp.ambient_mode", "ambientModeBackend", exposed("com.240mp.ambient_mode", ambientMode.get()), ctx);
    appCore.registerModule("com.240mp.nfc_reader",   "nfcReaderBackend",   exposed("com.240mp.nfc_reader", nfcReader.get()), ctx);
    appCore.registerModule("com.240mp.youtube",      "youtubeBackend",     exposed("com.240mp.youtube", youtubeBackend.get()), ctx);
    appCore.registerModule("com.240mp.weather",      "weatherBackend",     exposed("com.240mp.weather", weatherBackend.get()), ctx);
    appCore.registerModule("com.240mp.retroarch",    "retroarchBackend",   exposed("com.240mp.retroarch", retroArch.get()), ctx);
    appCore.registerModule("com.240mp.scripts",      "scriptsBackend",     exposed("com.240mp.scripts", scriptsBackend.get()), ctx);
    appCore.registerModule("com.240mp.virtual_channels", "virtualChannelsBackend", exposed("com.240mp.virtual_channels", virtualChannels.get()), ctx);

    // YouTube resolve telemetry: which resolver served a pair (innertube /
    // ytdlp / cache / negative-cache) and how long it took. Lands in the Pi
    // state log as
    // `yt-resolve vid=… source=… ms=…`; AppCore::note_state is compiled out of
    // release builds, so the cost is one signal emission.
    if (youtubeBackend) {
        QObject::connect(youtubeBackend.get(), &YouTubeBackend::resolveTelemetry, &appCore,
                         [&appCore](const QString &videoId, const QString &source, qint64 ms) {
                             appCore.note_state("yt-resolve",
                                                QStringLiteral("vid=%1 source=%2 ms=%3")
                                                    .arg(videoId, source).arg(ms));
                         });
    }

    // Idle SponsorBlock fill: the Pi is on all day, so confirm segment windows
    // while nobody is pressing keys. Driven by idleSeconds, NOT by the
    // screensaver's `active`/`enabled` — the owner may run with the screensaver
    // OFF and still want the cache filled. Any input activity stops it at once,
    // so a foreground press never queues behind a background fetch.
    constexpr int kIdleFillAfterSec = 60;
    if (youtubeBackend) {
        YouTubeBackend *yt = youtubeBackend.get();
        QObject::connect(&idleTracker, &IdleTracker::idleSecondsChanged, yt,
                         [&idleTracker, yt]() {
                             if (idleTracker.idleSeconds() >= kIdleFillAfterSec)
                                 yt->setIdleFillActive(true);
                         });
        QObject::connect(&idleTracker, &IdleTracker::activityDetected, yt,
                         [yt]() { yt->setIdleFillActive(false); });
    }

    ctx->setContextProperty("idleTracker",   &idleTracker);
    ctx->setContextProperty("appCore",       &appCore);
    ctx->setContextProperty("mpvController", &mpvController);
    ctx->setContextProperty("inputManager",  &inputManager);
    ctx->setContextProperty("updateManager", &updateManager);
    ctx->setContextProperty("volumeManager", &volumeManager);
#ifdef Q_OS_MAC
    // Target display geometry in Qt coordinates (top-left origin), so the QML
    // Window bindings position onto the chosen screen. The native fullscreen
    // call after load then nails the exact frame in AppKit coordinates.
    // QVariant(...) not a literal — a bare 0 is a null pointer constant and
    // resolves to the QObject* overload, handing QML null instead of an int.
    engine.rootContext()->setContextProperty("macScreenX",      QVariant(screenGeo.x()));
    engine.rootContext()->setContextProperty("macScreenY",      QVariant(screenGeo.y()));
    engine.rootContext()->setContextProperty("macScreenWidth",  QVariant(screenGeo.width()));
    engine.rootContext()->setContextProperty("macScreenHeight", QVariant(screenGeo.height()));
#endif

    engine.addImportPath(appRoot + "/views");

    engine.load(QUrl::fromLocalFile(appRoot + "/Main.qml"));
    if (engine.rootObjects().isEmpty()) {
        qCritical("[main] QML engine failed to load Main.qml");
        return 1;
    }

    // Gamepad key events are posted straight to the root window so they reach
    // the QML focus item even when another window (mpv) holds OS focus.
    auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    inputManager.setTargetWindow(rootWindow);

    //
    // The hand-off brackets the app window, not just the DRM device: on this Pi
    // Qt EGLFS never installed a VT_PROCESS handler (the app holds no console fd),
    // so the kernel VT switch does not stop it drawing. Hiding the window is what
    // actually suspends rendering while a child owns the screen; see the
    // displayTaken comment in DisplayHandoff.h. MP240_HANDOFF_KEEP_WINDOW=1 skips
    // it, for A/B testing the old behaviour without a rebuild — the predicate
    // lives in util/HandoffWindowPolicy.h so it can be unit-tested.
    if (rootWindow) {
        // One value drives both the hide and the "EGLFS re-initialises on the way
        // back" flag, so the two can never disagree.
        const bool hidesWindow = handoffwindow::hideWindowOnTake();
        // The restore path has to know whether EGLFS is being re-initialised on
        // the way back — see DisplayHandoff::setRendererSuspendsWindow.
        displayHandoff.setRendererSuspendsWindow(hidesWindow);
        QObject::connect(&displayHandoff, &DisplayHandoff::displayTaken,
                         rootWindow, [rootWindow, hidesWindow] {
                             if (hidesWindow) rootWindow->setVisible(false);
                         });

        // Showing the window makes EGLFS rebuild its swapchain and set the mode,
        // so the VT switch back waits for the frame that proves that is done. A
        // timer inside DisplayHandoff covers the case where no frame arrives.
        QObject::connect(&displayHandoff, &DisplayHandoff::displayReturned,
                         rootWindow, [rootWindow, &displayHandoff] {
                             rootWindow->setVisible(true);
                             rootWindow->requestUpdate();
                             // Hand the handle to DisplayHandoff so completeRestore()
                             // can drop it on the timer-wins path (VT-01).
                             displayHandoff.setFrameSwappedConnection(
                                 QObject::connect(rootWindow, &QQuickWindow::frameSwapped,
                                                  &displayHandoff,
                                                  &DisplayHandoff::completeRestore,
                                                  Qt::SingleShotConnection));
                         });
    }

    // eventFilter() must be installed on the window or it never runs — without
    // this, the remap/capture/media/F12/game-suppression channels are dead code
    // and raw OS keys fall straight through to QML (the exit-after-game stack bug
    // lived here: the child game already consumed those very keys).
    if (auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first()))
        rootWindow->installEventFilter(&inputManager);

    // Screenshot debug capture grabs through this window (see AppCore::captureScreenshot).
    appCore.setQuickWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));

    // Auto-capture debug mode (#43 beta accelerators): MP240_AUTOCAP_S=<secs>
    // (>=5) snapshots every N seconds on a side thread — continuous visual QA
    // while the agent drives, without manual Tab presses. Debug-only: readback
    // races the render loop on EGLFS (accepted for debug captures), and the
    // frame during mpv video playback is near-black (known framebuffer limit).
    // LIFECYCLE-01: owned (joinable) so the explicit teardown below can stop it
    // before the engine-owned window goes away. A detached loop calling into
    // AppCore was a pre-existing exit-time hazard in either declaration order.
    std::atomic<bool> autocapStop{false};
    std::thread autocapLoop;
    if (const char *ac = std::getenv("MP240_AUTOCAP_S")) {
        const int n = std::atoi(ac);
        if (n >= 5) {
            QQuickWindow *capWin = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (capWin) {
                autocapLoop = std::thread([n, &appCore, &autocapStop] {
                    while (!autocapStop.load()) {
                        for (int i = 0; i < n && !autocapStop.load(); ++i)
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        if (!autocapStop.load())
                            (void)appCore.takeScreenshot();
                    }
                });
                qInfo("[main] auto-capture every %d s (MP240_AUTOCAP_S)", n);
            }
        }
    }

#ifdef Q_OS_MAC
    if (QWindow *win = qobject_cast<QWindow *>(engine.rootObjects().first())) {
        // Move the window onto the target screen before forcing fullscreen, so
        // both Qt's and AppKit's notion of the window's screen agree.
        if (targetScreen)
            win->setScreen(targetScreen);
        win->setGeometry(screenGeo);
        win->winId(); // ensure native NSWindow is created
        forceWindowFullScreenOnScreen(reinterpret_cast<void *>(win->winId()), displayIndex);
    }
#elif defined(Q_OS_LINUX)
    // Desktop Linux (Steam Deck desktop mode, generic x86_64) only: EGLFS has
    // a single screen so displayIndex is already clamped to 0 there, but gate
    // on the platform name anyway so this can never disturb the Pi path.
    // Wayland ignores setGeometry (clients can't self-position); re-issuing
    // FullScreen after setScreen makes QtWayland send
    // xdg_toplevel.set_fullscreen(output) for the target.  On xcb the geometry
    // move plus the fullscreen re-request lands it the same way.
    if (displayIndex > 0 && targetScreen
        && (QGuiApplication::platformName() == QLatin1String("xcb")
            || QGuiApplication::platformName() == QLatin1String("wayland"))) {
        if (QWindow *win = qobject_cast<QWindow *>(engine.rootObjects().first())) {
            win->setScreen(targetScreen);
            win->setGeometry(screenGeo);
            win->setVisibility(QWindow::FullScreen);
        }
    }
#endif

    const int exitCode = app.exec();

    // LIFECYCLE-01 explicit teardown, while every singleton is still alive:
    // stop the debug capture loop and drop AppCore's raw pointer to the
    // engine-owned window. The engine destructs next (it is declared after the
    // singletons), so QML destruction handlers see live context properties.
    autocapStop.store(true);
    if (autocapLoop.joinable())
        autocapLoop.join();
    appCore.setQuickWindow(nullptr);

    return exitCode;
}
