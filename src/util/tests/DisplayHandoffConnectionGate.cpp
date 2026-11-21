// DisplayHandoffConnectionGate.cpp — VT-01 regression gate (checked-in).
// Healthy-contract gate for the frameSwapped -> completeRestore connection:
//   H1: <=1 armed connection per cycle; armed returns to 0 after every timer-won
//       cycle (no linear growth over 50 cycles);
//   H2: ZERO effective restore between acquire() and releaseDeferred() — no VT
//       switch back to the app VT, no owner clear, concurrent acquire refused;
//   H3: within-cycle idempotence holds;
//   ordering rules 1/2/3 unchanged (capture->announce; show while parked, switch
//       back only after frameSwapped; legacy CRTC restore skipped when EGLFS
//       re-initialises on show).
//
// Arms: sanity timer-wins frame-wins stale-frame-keepwindow inflight-hide
//       inflight-after-accumulation benign-after-return idempotence
//       ordering-hide ordering-keepwindow
// Exit 0 = arm PASS (contract holds), 1 = FAIL. timer-wins takes [cycles],
// default 50 (~40 s: 50 x the product's real 800 ms restore timer).
//
// Linux-only (VT ioctls / linux/vt.h); registered by the root CMakeLists under
// UNIX AND NOT APPLE. Runs offscreen with the software scene graph; needs no
// /dev/dri and no real VT: VT ioctls and libdrm are interposed at link time and
// the sysfs active-VT read is faked (APP_VT=1 on every host). Frames are emitted
// manually — exposeEvent is swallowed and UpdateRequest dropped, so the window
// never auto-renders and cannot steal a timer-won cycle.
//
// All metrics come from the interposer VT log, the event log and the armed
// counter — never from qDebug text — so the gate behaves identically in Debug
// and in CI's Release builds (QT_NO_DEBUG_OUTPUT strips qDebug there).
//
// Wiring: replica of src/main.cpp's displayReturned lambda in the VT-01-fixed
// shape — the frameSwapped->completeRestore SingleShotConnection handle is
// handed to DisplayHandoff::setFrameSwappedConnection so completeRestore()
// drops it at its tail (after the switch+clear). Two-sided validation history:
// with the pre-fix wiring (handle discarded) the arms timer-wins,
// stale-frame-keepwindow, inflight-hide, inflight-after-accumulation and
// benign-after-return FAIL (armed grows 1..50; premature restore + owner clear
// + intruder takeover); with the fix, 10/10 PASS. Derivation: VT-01 torture
// triage, kanban t_b3c85389 -> t_3da79814; see references/display-handoff.md.
#include <QGuiApplication>
#include <QQuickWindow>
#include <QDir>
#include <QExposeEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QMetaObject>
#include <QMetaMethod>
#include <QDebug>
#include <QTimer>
#include <QString>
#include <QByteArray>

#include <dlfcn.h>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <linux/vt.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "DisplayHandoff.h"
#include "HandoffWindowPolicy.h"

static constexpr int APP_VT = 1;    // faked sysfs active = "tty1" on every host
static constexpr int CHILD_VT = 7;  // interposer VT_OPENQRY hands back 7

// ---------------------------------------------------------------------------
// Unified sequence counter: VT ioctl log AND event log share g_seq, so
// cross-domain ordering (capture vs announce, frame vs switch-back) is total.
// ---------------------------------------------------------------------------
struct VtCall { int seq; unsigned long req; int vt; };
static std::vector<VtCall> g_vt;
struct Ev { int seq; const char *tag; int val; };
static std::vector<Ev> g_ev;
static std::set<int> g_fakeFds;
static int g_seq = 0;
static int g_fakeDrmFd = -1;          // ordering arms only
static std::string g_sysfsActivePath; // temp file containing "tty1\n"

static void ev(const char *tag, int val = 0) { g_ev.push_back({++g_seq, tag, val}); }
static int lastEv(const char *tag) {
    for (auto it = g_ev.rbegin(); it != g_ev.rend(); ++it)
        if (std::strcmp(it->tag, tag) == 0) return it->seq;
    return -1;
}
static int countEv(const char *tag, int afterSeq = -1) {
    int n = 0;
    for (const auto &e : g_ev)
        if (std::strcmp(e.tag, tag) == 0 && e.seq > afterSeq) ++n;
    return n;
}
static int backSwitchesSince(int seq) {
    int n = 0;
    for (const auto &c : g_vt)
        if (c.seq > seq && c.req == VT_ACTIVATE && c.vt == APP_VT) ++n;
    return n;
}
static int lastBackSwitchSeq() {
    for (auto it = g_vt.rbegin(); it != g_vt.rend(); ++it)
        if (it->req == VT_ACTIVATE && it->vt == APP_VT) return it->seq;
    return -1;
}
// Effective restores, counted from the VT log — NOT from qDebug text: release
// builds compile qDebug out (-DQT_NO_DEBUG_OUTPUT in CMAKE_CXX_FLAGS_RELEASE).
// Equivalence: with the interposer every acquire switches VT successfully, so
// each guarded completeRestore performs exactly one VT_ACTIVATE to APP_VT, and
// VT_ACTIVATE to APP_VT occurs nowhere else (acquire switches to CHILD_VT).
static int effectiveRestores() {
    int n = 0;
    for (const auto &c : g_vt)
        if (c.req == VT_ACTIVATE && c.vt == APP_VT) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Link-time interposer: fake /dev/tty0 + /dev/ttyN opens and VT ioctls, fake
// the sysfs active-VT read, and (ordering arms) one DRM primary fd. Only fds we
// handed out get fake handling; everything else passes through to libc/libdrm.
// ---------------------------------------------------------------------------
using open_fn  = int (*)(const char *, int, ...);
using ioctl_fn = int (*)(int, unsigned long, ...);
using close_fn = int (*)(int);
using fstat_fn = int (*)(int, struct stat *);
using fstat64_fn = int (*)(int, struct stat64 *);
static open_fn  real_open  = nullptr;
static open_fn  real_open64 = nullptr;
static ioctl_fn real_ioctl = nullptr;
static close_fn real_close = nullptr;
static fstat_fn real_fstat = nullptr;
static fstat64_fn real_fstat64 = nullptr;

static void initReal() {
    if (!real_open)   real_open   = (open_fn)dlsym(RTLD_NEXT, "open");
    if (!real_open64) real_open64 = (open_fn)dlsym(RTLD_NEXT, "open64");
    if (!real_ioctl)  real_ioctl  = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    if (!real_close)  real_close  = (close_fn)dlsym(RTLD_NEXT, "close");
    if (!real_fstat)  real_fstat  = (fstat_fn)dlsym(RTLD_NEXT, "fstat");
    if (!real_fstat64) real_fstat64 = (fstat64_fn)dlsym(RTLD_NEXT, "fstat64");
}
static bool isTtyPath(const char *p) {
    if (!p) return false;
    if (std::strcmp(p, "/dev/tty0") == 0) return true;
    if (std::strncmp(p, "/dev/tty", 8) == 0) {
        const char *s = p + 8;
        if (*s == 0) return false;            // bare /dev/tty = controlling tty
        for (const char *c = s; *c; ++c)
            if (*c < '0' || *c > '9') return false;
        return true;
    }
    return false;
}
static int fakeTtyOpen() {
    int fd = real_open("/dev/null", O_WRONLY);
    if (fd >= 0) g_fakeFds.insert(fd);
    return fd;
}
static bool isSysfsActive(const char *p) {
    return p && std::strcmp(p, "/sys/class/tty/tty0/active") == 0;
}
extern "C" int open(const char *path, int flags, ...) {
    initReal();
    if (isTtyPath(path)) return fakeTtyOpen();
    if (isSysfsActive(path)) return real_open(g_sysfsActivePath.c_str(), O_RDONLY);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
        return real_open(path, flags, m);
    }
    return real_open(path, flags);
}
extern "C" int open64(const char *path, int flags, ...) {
    initReal();
    if (isTtyPath(path)) return fakeTtyOpen();
    if (isSysfsActive(path)) return real_open(g_sysfsActivePath.c_str(), O_RDONLY);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
        return real_open64(path, flags, m);
    }
    return real_open64(path, flags);
}
extern "C" int ioctl(int fd, unsigned long req, ...) {
    initReal();
    va_list ap; va_start(ap, req);
    if (g_fakeFds.count(fd)) {
        if (req == VT_ACTIVATE || req == VT_WAITACTIVE) {
            int vt = va_arg(ap, int); va_end(ap);
            g_vt.push_back({++g_seq, req, vt});
            return 0;
        }
        if (req == VT_OPENQRY) {
            int *p = va_arg(ap, int *); va_end(ap);
            if (p) *p = CHILD_VT;
            g_vt.push_back({++g_seq, req, CHILD_VT});
            return 0;
        }
    }
    if (fd == g_fakeDrmFd && fd >= 0 &&
        (req == DRM_IOCTL_SET_MASTER || req == DRM_IOCTL_DROP_MASTER)) {
        va_end(ap);
        return 0;   // fake DRM master ops succeed
    }
    void *arg = va_arg(ap, void *); va_end(ap);
    return real_ioctl(fd, req, arg);
}
extern "C" int close(int fd) {
    initReal();
    g_fakeFds.erase(fd);
    return real_close(fd);
}
// findQtDrmFd() fstats every /proc/self/fd entry looking for major 226, minor<64.
// Present our /dev/null-backed fd as DRM primary card0 so the no-hook capture
// path runs offscreen. All other fds pass through untouched.
template <typename StatT, typename RealFn>
static int fakeDrmStat(int fd, StatT *st, RealFn real) {
    if (fd == g_fakeDrmFd && fd >= 0 && st) {
        std::memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = makedev(226, 0);
        return 0;
    }
    return real(fd, st);
}
extern "C" int fstat(int fd, struct stat *st) {
    initReal();
    return fakeDrmStat(fd, st, real_fstat);
}
extern "C" int fstat64(int fd, struct stat64 *st) {
    initReal();
    return fakeDrmStat(fd, st, real_fstat64);
}

// ---------------------------------------------------------------------------
// Fake libdrm capture/restore (ordering arms). Static fixtures; the Free*
// functions are no-ops because the fixtures are not heap-allocated.
// drmModeSetCrtc is the rule-3 observable: counted, never really called.
// ---------------------------------------------------------------------------
static int g_setCrtcCalls = 0;
static drmModeRes fakeRes;
static uint32_t fakeCrtcs[1] = {42}, fakeConns[1] = {5}, fakeEncs[1] = {9};
static drmModeCrtc fakeCrtc;
static drmModeConnector fakeConn;
static drmModeEncoder fakeEnc;
static drmModeModeInfo fakeMode;
static void initFakeDrm() {
    std::memset(&fakeRes, 0, sizeof(fakeRes));
    fakeRes.count_crtcs = 1;      fakeRes.crtcs = fakeCrtcs;
    fakeRes.count_connectors = 1; fakeRes.connectors = fakeConns;
    fakeRes.count_encoders = 1;   fakeRes.encoders = fakeEncs;
    std::memset(&fakeCrtc, 0, sizeof(fakeCrtc));
    fakeCrtc.crtc_id = 42; fakeCrtc.buffer_id = 7; fakeCrtc.mode_valid = 1;
    std::memset(&fakeMode, 0, sizeof(fakeMode));
    fakeMode.hdisplay = 640; fakeMode.vdisplay = 480; fakeMode.vrefresh = 60;
    fakeMode.clock = 25175;
    fakeCrtc.mode = fakeMode;
    std::memset(&fakeConn, 0, sizeof(fakeConn));
    fakeConn.connector_id = 5; fakeConn.encoder_id = 9;
    fakeConn.connection = DRM_MODE_CONNECTED;
    std::memset(&fakeEnc, 0, sizeof(fakeEnc));
    fakeEnc.encoder_id = 9; fakeEnc.crtc_id = 42;
}
extern "C" drmModeResPtr drmModeGetResources(int) { ev("capture"); return &fakeRes; }
extern "C" drmModeCrtcPtr drmModeGetCrtc(int, uint32_t) { return &fakeCrtc; }
extern "C" drmModeConnectorPtr drmModeGetConnector(int, uint32_t) { return &fakeConn; }
extern "C" drmModeEncoderPtr drmModeGetEncoder(int, uint32_t) { return &fakeEnc; }
extern "C" void drmModeFreeResources(drmModeResPtr) {}
extern "C" void drmModeFreeCrtc(drmModeCrtcPtr) {}
extern "C" void drmModeFreeConnector(drmModeConnectorPtr) {}
extern "C" void drmModeFreeEncoder(drmModeEncoderPtr) {}
extern "C" int drmModeSetCrtc(int, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t *, int, drmModeModeInfoPtr) {
    ++g_setCrtcCalls;
    ev("setcrtc");
    return 0;
}

// ---------------------------------------------------------------------------
// Observables
// ---------------------------------------------------------------------------
static int g_qdebugRestores = 0;      // diagnostic cross-check only (Debug builds)
static int g_skipLines = 0;           // "CRTC restore skipped" qDebug lines (rule 3, Debug only)
static int g_taken = 0, g_returned = 0;
static bool g_visibleAtReturned = false;
static int g_backSwitchesAtReturned = -1;

static void msgHandler(QtMsgType, const QMessageLogContext &, const QString &msg) {
    if (msg.contains(QStringLiteral("Switching back to VT")))
        ++g_qdebugRestores;
    if (msg.contains(QStringLiteral("CRTC restore skipped")))
        ++g_skipLines;
    std::fprintf(stderr, "%s\n", qPrintable(msg));
}

class TestWindow : public QQuickWindow {
public:
    int armed = 0, connects = 0, disconnects = 0;
    void swapFrame() { ev("frame"); emit frameSwapped(); }
    void swapFrameQueued() {
        QMetaObject::invokeMethod(this, [this] { ev("frame"); emit frameSwapped(); },
                                  Qt::QueuedConnection);
    }
protected:
    // Swallow expose + drop UpdateRequest so the window NEVER auto-renders:
    // timer-wins vs frame-wins stays deterministic (see spec flake section).
    void exposeEvent(QExposeEvent *) override {}
    bool event(QEvent *e) override {
        if (e->type() == QEvent::UpdateRequest) return true;
        return QQuickWindow::event(e);
    }
    void connectNotify(const QMetaMethod &m) override {
        if (m.name() == QByteArrayLiteral("frameSwapped")) { ++armed; ++connects; }
    }
    void disconnectNotify(const QMetaMethod &m) override {
        if (m.name() == QByteArrayLiteral("frameSwapped")) { --armed; ++disconnects; }
    }
};

// Wiring replica of src/main.cpp's displayReturned lambda, VT-01-fixed shape.
// show/hide event marks are harness observers, not wiring changes.
static void wireMainCpp(DisplayHandoff &h, TestWindow &w, bool hidesWindow) {
    h.setRendererSuspendsWindow(hidesWindow);
    QObject::connect(&h, &DisplayHandoff::displayTaken, &w, [&w, hidesWindow] {
        if (hidesWindow) { w.setVisible(false); ev("hide"); }
    });
    QObject::connect(&h, &DisplayHandoff::displayReturned, &w, [&w, &h] {
        w.setVisible(true); ev("show");
        w.requestUpdate();
        // VT-01-fixed wiring (matches src/main.cpp): hand over the handle so
        // completeRestore() can drop it on the timer-wins path.
        h.setFrameSwappedConnection(
            QObject::connect(&w, &QQuickWindow::frameSwapped, &h,
                             &DisplayHandoff::completeRestore, Qt::SingleShotConnection));
    });
    // Harness spies (connected AFTER the replica so 'show' already ran).
    QObject::connect(&h, &DisplayHandoff::displayTaken, &h, [&h] {
        ++g_taken; ev("taken");
    }, Qt::DirectConnection);
    QObject::connect(&h, &DisplayHandoff::displayReturned, &h, [&h, &w] {
        ++g_returned;
        g_visibleAtReturned = w.isVisible();
        g_backSwitchesAtReturned = backSwitchesSince(lastEv("taken"));
        ev("returned");
    }, Qt::DirectConnection);
}

static void pumpFor(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
static bool pumpUntil(const std::function<bool()> &pred, int timeoutMs) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (pred()) return true;
        QThread::msleep(1);
    }
    return pred();
}

static int g_failures = 0;
static void check(const char *name, bool ok, const QString &detail = QString()) {
    std::printf("CHECK %-58s %s%s%s\n", name, ok ? "ok  " : "FAIL",
                detail.isEmpty() ? "" : "  | ", qPrintable(detail));
    if (!ok) ++g_failures;
}
static int take(DisplayHandoff &h, const QString &owner) { return h.acquire(owner); }
static bool returnAndWait(DisplayHandoff &h, const QString &owner, int before) {
    h.releaseDeferred(owner, [] {}, 0);
    return pumpUntil([&] { return g_returned > before; }, 2000);
}
static bool waitTimerRestore(int e0) {
    return pumpUntil([&] { return effectiveRestores() > e0; }, 1500);
}

// ---------------------------------------------------------------------------
// G0 sanity — instrument two-sided canary (unchanged semantics from lab arm).
// ---------------------------------------------------------------------------
static void armSanity() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    int r0 = g_returned;
    check("sanity.take", take(h, "s") > 0);
    check("sanity.return", returnAndWait(h, "s", r0));
    check("sanity.armed==1 after one wire", w.armed == 1, QString("armed=%1").arg(w.armed));
    QObject::connect(&w, &QQuickWindow::frameSwapped, &h,
                     &DisplayHandoff::completeRestore, Qt::SingleShotConnection);
    check("sanity.detects injected double-wire (armed==2)", w.armed == 2,
          QString("armed=%1").arg(w.armed));
    int e0 = effectiveRestores();
    w.swapFrame();
    check("sanity.idempotent effective restore==1", effectiveRestores() - e0 == 1,
          QString("delta=%1").arg(effectiveRestores() - e0));
    check("sanity.armed==0 after frame consumed both", w.armed == 0,
          QString("armed=%1").arg(w.armed));
#ifndef QT_NO_DEBUG_OUTPUT
    check("sanity.VT-log restore count == qDebug count (metric equivalence)",
          effectiveRestores() == g_qdebugRestores,
          QString("vt=%1 qdebug=%2").arg(effectiveRestores()).arg(g_qdebugRestores));
#endif
}

// ---------------------------------------------------------------------------
// G1 timer-wins accumulation (promoted lab arm B, expectations INVERTED).
// Gate: armed==0 after EVERY timer-won cycle; <=1 armed per cycle; no growth
// over >=50 cycles; the cycle-51 frame fires <=1 connection.
// ---------------------------------------------------------------------------
static void armTimerWins(int cycles) {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    std::vector<int> armedPerCycle;
    bool ok = true;
    int maxArmedInWindow = 0;
    for (int c = 1; c <= cycles; ++c) {
        int r0 = g_returned, e0 = effectiveRestores();
        if (take(h, "t") <= 0) { ok = false; break; }
        if (!returnAndWait(h, "t", r0)) { ok = false; break; }
        maxArmedInWindow = std::max(maxArmedInWindow, w.armed);
        if (!waitTimerRestore(e0)) { ok = false; break; }   // NO frame: timer must win
        armedPerCycle.push_back(w.armed);
        int retSeq = lastEv("returned"), backSeq = lastBackSwitchSeq();
        bool timerWon = countEv("frame", retSeq) == 0 || lastEv("frame") < retSeq;
        if (w.armed != 0) ok = false;
        if (effectiveRestores() - e0 != 1) ok = false;
        if (!h.currentOwner().isEmpty()) ok = false;
        if (backSwitchesSince(retSeq) != 1) ok = false;
        if (!timerWon || backSeq < retSeq) ok = false;
    }
    QString series;
    for (size_t i = 0; i < armedPerCycle.size(); ++i) {
        if (i) series += ",";
        series += QString::number(armedPerCycle[i]);
        if (i >= 9 && armedPerCycle.size() > 12) {
            series += ",...,"; series += QString::number(armedPerCycle.back()); break;
        }
    }
    check(QString("G1.armed==0 after each of %1 timer-won cycles").arg(cycles).toUtf8().constData(),
          ok && (int)armedPerCycle.size() == cycles, QString("armed=[%1]").arg(series));
    check("G1.max armed during restore window <=1", maxArmedInWindow <= 1,
          QString("maxArmed=%1").arg(maxArmedInWindow));
    // Cycle N+1: one delivered frame must fire at most ONE connection.
    int r0 = g_returned, e0 = effectiveRestores();
    take(h, "t"); returnAndWait(h, "t", r0);
    int armedBeforeFrame = w.armed;
    w.swapFrame();
    pumpFor(5);
    check("G1.cycle-N+1 frame fires <=1 conn, exactly 1 effective restore",
          armedBeforeFrame <= 1 && effectiveRestores() - e0 == 1,
          QString("armedBeforeFrame=%1 restoreDelta=%2")
              .arg(armedBeforeFrame).arg(effectiveRestores() - e0));
    check("G1.armed==0 after the frame", w.armed == 0, QString("armed=%1").arg(w.armed));
}

// ---------------------------------------------------------------------------
// G2 frame-wins healthy control (promoted lab arm A). PASS on both sides.
// ---------------------------------------------------------------------------
static void armFrameWins() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    bool ok = true; int maxArmed = 0;
    for (int c = 1; c <= 10; ++c) {
        int r0 = g_returned, e0 = effectiveRestores();
        if (take(h, "a") <= 0) { ok = false; break; }
        if (!returnAndWait(h, "a", r0)) { ok = false; break; }
        maxArmed = std::max(maxArmed, w.armed);
        int takeSeq = lastEv("taken");
        w.swapFrame();                       // frame wins
        pumpFor(5);
        if (effectiveRestores() - e0 != 1) ok = false;
        if (w.armed != 0) ok = false;
        if (!h.currentOwner().isEmpty()) ok = false;
        if (backSwitchesSince(takeSeq) != 1) ok = false;
        if (lastBackSwitchSeq() < lastEv("frame")) ok = false;  // rule 2: switch AFTER frame
    }
    check("G2.frame-wins 10 cycles: 1 restore/cycle, armed==0 after, switch after frame", ok);
    check("G2.max armed during restore window <=1", maxArmed <= 1,
          QString("maxArmed=%1").arg(maxArmed));
}

// ---------------------------------------------------------------------------
// Shared H2 mid-handoff probe: a frame delivered while a handoff is actively
// held must cause ZERO effective restore, ZERO switch-back, and the owner must
// stay held (concurrency guard intact: an intruder acquire must be refused).
// Returns false if the defect fired (intruder took over); caller then stops.
// ---------------------------------------------------------------------------
static bool midHandoffFrameProbe(DisplayHandoff &h, TestWindow &w,
                                 const QString &owner, int takeSeq, bool queued) {
    int e1 = effectiveRestores();
    if (queued) {
        // frame was posted BEFORE take; wait for its delivery, then settle.
        if (!pumpUntil([&] { return lastEv("frame") > takeSeq; }, 2000)) {
            check("H2.queued frame delivered", false);
            return false;
        }
    } else {
        w.swapFrame();
    }
    pumpFor(5);
    check("H2.ZERO effective restore while handoff held",
          effectiveRestores() - e1 == 0,
          QString("restoreDelta=%1").arg(effectiveRestores() - e1));
    check("H2.owner still held after mid-handoff frame",
          h.currentOwner() == owner, QString("owner='%1'").arg(h.currentOwner()));
    check("H2.ZERO VT switch-back while handoff held",
          backSwitchesSince(takeSeq) == 0,
          QString("backSwitches=%1").arg(backSwitchesSince(takeSeq)));
    int iv = h.acquire(QStringLiteral("intruder"));
    check("H2.concurrent acquire refused while held (owner guard intact)", iv == -1,
          QString("intruderAcquire=%1").arg(iv));
    if (iv > 0) {
        // Defect side: the early owner clear let the intruder in. Clean up so
        // the arm ends in a sane state; the gate has already failed above.
        int r = g_returned;
        h.releaseDeferred(QStringLiteral("intruder"), [] {}, 0);
        pumpUntil([&] { return g_returned > r; }, 2000);
        w.swapFrame();
        pumpFor(5);
        return false;
    }
    return true;
}

// Legit release tail after a held handoff: return fires, owner still held until
// the post-return frame, then exactly one restore, switch after frame, armed 0.
static void releaseTail(DisplayHandoff &h, TestWindow &w, const QString &owner,
                        int takeSeq) {
    int r1 = g_returned, e1 = effectiveRestores();
    if (!returnAndWait(h, owner, r1)) { check("H2.release tail: displayReturned", false); return; }
    check("H2.release tail: owner held until post-return frame",
          h.currentOwner() == owner, QString("owner='%1'").arg(h.currentOwner()));
    w.swapFrame();
    pumpFor(5);
    check("H2.release tail: exactly 1 legit restore, owner cleared, armed==0",
          effectiveRestores() - e1 == 1 && h.currentOwner().isEmpty() && w.armed == 0,
          QString("restoreDelta=%1 armed=%2").arg(effectiveRestores() - e1).arg(w.armed));
    check("H2.release tail: exactly 1 switch-back, after the frame",
          backSwitchesSince(takeSeq) == 1 && lastBackSwitchSeq() > lastEv("frame"),
          QString("backSwitches=%1").arg(backSwitchesSince(takeSeq)));
}

// One timer-won cycle; the fixed restore tail must leave no stale conn armed.
static bool timerWonCycle(DisplayHandoff &h, const QString &owner) {
    int r0 = g_returned, e0 = effectiveRestores();
    if (take(h, owner) <= 0) return false;
    if (!returnAndWait(h, owner, r0)) return false;
    if (!waitTimerRestore(e0)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// G3 stale frame during a later handoff — KEEP_WINDOW debug world
// (promoted lab arm C, expectations INVERTED; Cfix is the passing shape).
// ---------------------------------------------------------------------------
static void armStaleKeepWindow() {
    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("1"));
    bool hides = handoffwindow::hideWindowOnTake();   // false in this world
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, hides);
    if (!timerWonCycle(h, "c1")) { check("G3.cycle1 timer-won", false); return; }
    check("G3.armed==0 after timer-won cycle 1", w.armed == 0, QString("armed=%1").arg(w.armed));
    take(h, "c2");
    check("G3.owner held before frame", h.currentOwner() == QStringLiteral("c2"));
    int takeSeq = g_seq;   // after cycle-2 away switch
    if (midHandoffFrameProbe(h, w, "c2", takeSeq, /*queued=*/false))
        releaseTail(h, w, "c2", takeSeq);
    qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
}

// ---------------------------------------------------------------------------
// G4 shipped hide-world in-flight frame (promoted lab arm D, INVERTED + full
// hold-window asserts + release tail; Dfix is the passing shape).
// ---------------------------------------------------------------------------
static void armInflightHide() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    if (!timerWonCycle(h, "d1")) { check("G4.cycle1 timer-won", false); return; }
    check("G4.armed==0 after timer-won cycle 1", w.armed == 0, QString("armed=%1").arg(w.armed));
    w.swapFrameQueued();                 // in flight at take time
    take(h, "d2");                       // sync: away switch, capture, hide
    int takeSeq = g_seq;
    check("G4.owner held right after take", h.currentOwner() == QStringLiteral("d2"));
    if (midHandoffFrameProbe(h, w, "d2", takeSeq, /*queued=*/true))
        releaseTail(h, w, "d2", takeSeq);
}

// ---------------------------------------------------------------------------
// G5 NEW: shipped hide-world in-flight frame AFTER accumulation — 5 timer-won
// cycles, then a queued frame at take of cycle 6. No lab arm combined B and D.
// ---------------------------------------------------------------------------
static void armInflightAfterAccumulation() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    for (int c = 1; c <= 5; ++c) {
        if (!timerWonCycle(h, QStringLiteral("k%1").arg(c))) {
            check("G5.timer-won warmup cycles", false); return;
        }
    }
    check("G5.armed==0 after 5 timer-won cycles (no accumulation)", w.armed == 0,
          QString("armed=%1").arg(w.armed));
    w.swapFrameQueued();
    take(h, "k6");
    int takeSeq = g_seq;
    check("G5.owner held right after take", h.currentOwner() == QStringLiteral("k6"));
    if (midHandoffFrameProbe(h, w, "k6", takeSeq, /*queued=*/true))
        releaseTail(h, w, "k6", takeSeq);
}

// ---------------------------------------------------------------------------
// G6 benign common path (promoted Dbenign): frame only AFTER displayReturned.
// Effective-restore behavior is benign even on the defect, but the strict
// armed<=1-per-cycle invariant still fails there (2 armed at frame time).
// ---------------------------------------------------------------------------
static void armBenignAfterReturn() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    if (!timerWonCycle(h, "e1")) { check("G6.cycle1 timer-won", false); return; }
    int r1 = g_returned, e1 = effectiveRestores();
    take(h, "e2");
    int takeSeq = g_seq;
    if (!returnAndWait(h, "e2", r1)) { check("G6.cycle2 return", false); return; }
    check("G6.owner held before post-return frame", h.currentOwner() == QStringLiteral("e2"));
    check("G6.armed<=1 during cycle-2 restore window", w.armed <= 1,
          QString("armed=%1").arg(w.armed));
    w.swapFrame();                       // frame AFTER return: legit restore
    pumpFor(5);
    check("G6.benign: exactly 1 restore after return, owner cleared, armed==0",
          effectiveRestores() - e1 == 1 && h.currentOwner().isEmpty() && w.armed == 0,
          QString("restoreDelta=%1 armed=%2").arg(effectiveRestores() - e1).arg(w.armed));
    check("G6.switch-back after the frame, exactly 1",
          backSwitchesSince(takeSeq) == 1 && lastBackSwitchSeq() > lastEv("frame"));
}

// ---------------------------------------------------------------------------
// G7 within-cycle idempotence (promoted lab arm E; H3). PASS on both sides.
// The checked-in display-handoff-signal ctest remains the second H3 witness.
// ---------------------------------------------------------------------------
static void armIdempotence() {
    DisplayHandoff h; TestWindow w;
    h.setCrtcCaptureHookForTest([] {});
    wireMainCpp(h, w, true);
    int r0 = g_returned, e0 = effectiveRestores();
    take(h, "f"); returnAndWait(h, "f", r0);
    w.swapFrame();                                   // completeRestore #1
    pumpFor(5);
    size_t vt0 = g_vt.size();
    h.completeRestore();                             // #2
    h.completeRestore();                             // #3
    pumpFor(5);
    check("G7.idempotent: no extra VT switch after state clear", g_vt.size() == vt0,
          QString("vtCallsDelta=%1").arg(g_vt.size() - vt0));
    check("G7.idempotent: effective restore stays 1, return stays 1",
          effectiveRestores() - e0 == 1 && g_returned - r0 == 1,
          QString("restoreDelta=%1 returnDelta=%2")
              .arg(effectiveRestores() - e0).arg(g_returned - r0));
    check("G7.armed==0 after frame consumed the conn", w.armed == 0,
          QString("armed=%1").arg(w.armed));
}

// ---------------------------------------------------------------------------
// G8/G9 ordering rules (NEW arms, both worlds). No capture hook: the fake DRM
// fd makes the REAL capture/restore code path run against interposed libdrm.
//   rule 1: capture before announce (event seq)
//   rule 2: show while parked (visible at displayReturned, zero switch-back
//           between take and return), switch back only after frameSwapped
//   rule 3: hide world -> "CRTC restore skipped" line + ZERO drmModeSetCrtc;
//           KEEP_WINDOW world -> exactly ONE drmModeSetCrtc per cycle
// These are invariant arms: PASS on defect AND fixed (they catch a fix that
// reorders the rules, not the connection defect itself).
// ---------------------------------------------------------------------------
static void armOrdering(bool keepWindow) {
    if (keepWindow)
        qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("1"));
    else
        qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
    bool hides = handoffwindow::hideWindowOnTake();
    DisplayHandoff h; TestWindow w;
    // NO capture hook — real findQtDrmFd/saveDrmCrtcState/doRestore path.
    g_fakeDrmFd = ::open("/dev/null", O_WRONLY);   // presented as DRM card0 via fstat
    wireMainCpp(h, w, hides);
    w.setVisible(true);                            // app window is up before any take
    bool ok = true;
    for (int c = 1; c <= 2; ++c) {
        int e0 = effectiveRestores(), k0 = g_setCrtcCalls;
#ifndef QT_NO_DEBUG_OUTPUT
        int s0 = g_skipLines;
#endif
        int r0 = g_returned;
        int vt = take(h, QStringLiteral("o%1").arg(c));
        if (vt <= 0) { ok = false; break; }
        int capSeq = lastEv("capture"), takeSeqEv = lastEv("taken");
        if (capSeq < 0 || takeSeqEv < 0 || capSeq >= takeSeqEv) ok = false;  // rule 1
        if (hides && w.isVisible()) ok = false;          // hidden while held
        if (!hides && !w.isVisible()) ok = false;        // kept up while held
        int takeSeq = g_seq;
        if (!returnAndWait(h, QStringLiteral("o%1").arg(c), r0)) { ok = false; break; }
        if (!g_visibleAtReturned) ok = false;            // rule 2: shown while parked
        if (g_backSwitchesAtReturned != 0) ok = false;   // rule 2: still parked at return
        w.swapFrame();
        pumpFor(5);
        if (effectiveRestores() - e0 != 1) ok = false;
        if (backSwitchesSince(takeSeq) != 1) ok = false;
        if (lastBackSwitchSeq() < lastEv("frame")) ok = false;  // rule 2: after frame
        if (w.armed != 0) ok = false;
        if (!h.currentOwner().isEmpty()) ok = false;
        if (hides) {                                     // rule 3: skip when EGLFS re-inits
#ifndef QT_NO_DEBUG_OUTPUT
            if (g_skipLines - s0 != 1) ok = false;       // qDebug corroboration (Debug only)
#endif
            if (g_setCrtcCalls - k0 != 0) ok = false;    // primary: zero legacy modesets
        } else {                                         // rule 3: legacy restore in play
#ifndef QT_NO_DEBUG_OUTPUT
            if (g_skipLines - s0 != 0) ok = false;
#endif
            if (g_setCrtcCalls - k0 != 1) ok = false;    // primary: one legacy modeset/cycle
        }
    }
    const char *world = keepWindow ? "keepwindow" : "hide";
    check(QString("G8/G9.ordering rules 1+2+3 hold in %1 world (2 cycles)").arg(world)
              .toUtf8().constData(), ok,
          QString("skipLines=%1 setCrtcCalls=%2 taken=%3 returned=%4")
              .arg(g_skipLines).arg(g_setCrtcCalls).arg(g_taken).arg(g_returned));
    check("G8/G9.exactly one capture and one announce per cycle",
          countEv("capture") == 2 && g_taken == 2 && g_returned == 2);
    if (g_fakeDrmFd >= 0) { ::close(g_fakeDrmFd); g_fakeDrmFd = -1; }
    if (keepWindow)
        qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
}

int main(int argc, char **argv) {
    qInstallMessageHandler(msgHandler);
    QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    QGuiApplication app(argc, argv);
    initFakeDrm();
    // Deterministic app VT on every host: fake the sysfs read production uses.
    QByteArray tmpl = (QDir::tempPath() + QStringLiteral("/vt01gate-sysfs-XXXXXX")).toLocal8Bit();
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back('\0');
    int sfd = mkstemp(tbuf.data());
    if (sfd < 0) { std::printf("cannot create sysfs fixture\n"); return 2; }
    if (write(sfd, "tty1\n", 5) != 5) { std::printf("sysfs fixture write failed\n"); return 2; }
    ::close(sfd);
    g_sysfsActivePath = tbuf.data();

    QString arm = argc > 1 ? argv[1] : QStringLiteral("sanity");
    int cycles = argc > 2 ? std::atoi(argv[2]) : 50;
    const char *mode = "checked-in-design-a";
    std::printf("=== VT-01 gate arm %s (cycles=%d, wiring=%s) ===\n",
                qPrintable(arm), cycles, mode);
    if (arm == "sanity") armSanity();
    else if (arm == "timer-wins") armTimerWins(cycles);
    else if (arm == "frame-wins") armFrameWins();
    else if (arm == "stale-frame-keepwindow") armStaleKeepWindow();
    else if (arm == "inflight-hide") armInflightHide();
    else if (arm == "inflight-after-accumulation") armInflightAfterAccumulation();
    else if (arm == "benign-after-return") armBenignAfterReturn();
    else if (arm == "idempotence") armIdempotence();
    else if (arm == "ordering-hide") armOrdering(false);
    else if (arm == "ordering-keepwindow") armOrdering(true);
    else { std::printf("unknown arm\n"); return 2; }
    std::printf("VT_CALLS %zu\n", g_vt.size());
    for (const auto &c : g_vt)
        std::printf("  VT seq=%d req=%lu vt=%d\n", c.seq, c.req, c.vt);
    std::printf("ARM_RESULT %s failures=%d effectiveRestores=%d taken=%d returned=%d "
                "setCrtcCalls=%d skipLines=%d qdebugRestores=%d\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures, effectiveRestores(),
                g_taken, g_returned, g_setCrtcCalls, g_skipLines, g_qdebugRestores);
    ::unlink(g_sysfsActivePath.c_str());
    return g_failures == 0 ? 0 : 1;
}
