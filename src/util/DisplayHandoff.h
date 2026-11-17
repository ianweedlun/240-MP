#pragma once
#include <QObject>
#include <QString>
#include <functional>

class QTimer;

#ifdef Q_OS_LINUX
#include <xf86drm.h>
#include <xf86drmMode.h>

struct DrmSavedState {
    uint32_t crtcId      = 0;
    uint32_t connectorId = 0;
    uint32_t fbId        = 0;
    int      x           = 0;
    int      y           = 0;
    drmModeModeInfo mode = {};
    bool     valid       = false;
};
#endif

// Brackets a fullscreen external process on headless Linux (RPi Lite / EGLFS),
// where Qt owns the DRM device and a child that wants the screen can't simply
// open a window over ours.
//
// One instance owns the hand-off for the whole app, so two subsystems can never
// both believe they hold the screen: acquire() takes an owner token and refuses
// if a different owner already holds it.
//
// The ORDER HERE IS LOAD-BEARING and was established against real Pi hardware —
// do not reorder it, and do not re-implement these ioctls in a new caller:
//
//   acquire():  VT switch  ->  drmDropMaster  ->  save CRTC state
//     The VT switch goes FIRST because it suspends Qt's render thread via the
//     kernel's VT-switch signal before DRM master is dropped. On kernels 5.8+
//     drmSetMaster() returns EACCES for non-root while any other process holds
//     master, and Qt EGLFS runs VT_AUTO and never calls drmDropMaster() itself.
//
//   release():  drmSetMaster  ->  restore CRTC  ->  VT switch back
//     Exactly the inverse. The CRTC restore uses LEGACY drmModeSetCrtc, not an
//     atomic commit: the child's atomic cleanup leaves CRTC_ACTIVE=0, and EGLFS
//     then gets EINVAL on its first page flip.
//     The default 200 ms defer lets the child's last pending KMS commit clear in
//     the vc4 driver; commit too early and EGLFS gets EBUSY repeatedly, drops its
//     DRM pipeline, and the kernel falls back to the text console on Qt's VT.
//     200 ms is more than three VSync periods at 60 Hz.
//
// On macOS and on desktop Linux (X11/Wayland present) there is nothing to hand
// off — the child just opens a window over ours — so acquire() is a no-op that
// returns 0 and every other call short-circuits.
class DisplayHandoff : public QObject {
    Q_OBJECT
public:
    explicit DisplayHandoff(QObject *parent = nullptr);
    ~DisplayHandoff() override;

    // True when Qt owns the display directly (EGLFS/DRM) rather than through a
    // compositor, which is the only case that needs a hand-off.
    static bool isHeadless();

    // True only while this owner holds a hand-off that has a VT to return to.
    // Callers use it as a re-entrancy guard: acquiring twice without releasing
    // would overwrite the saved VT with the free one we just switched to.
    bool    isHeldBy(const QString &owner) const;
    QString currentOwner() const { return m_owner; }

    // False when the CRTC state captured by acquire() is unusable, meaning the
    // post-exit restore will NOT be able to put the display back. A caller that
    // can decline (a script launcher, which would otherwise strand the user on a
    // black screen with no way home) should check this and refuse; mpv
    // deliberately does not, because playback has always proceeded in this case.
    //
    // ONLY MEANINGFUL WHEN acquire() RETURNED > 0. It is also false on macOS and
    // on desktop Linux, where there is no CRTC to save and nothing to restore —
    // treating that as "refuse" would break every non-headless target.
    bool    savedStateValid() const;

    // Returns the VT the child now has to itself (>0), or 0 when no hand-off was
    // needed (non-Linux, or a compositor is present). Returns -1 when refused
    // because a different owner already holds the screen.
    int  acquire(const QString &owner);

    // Restores the display after delayMs, then invokes onRestored. The callback
    // always runs — even if this owner never held the screen — because callers
    // use it to emit their "finished" signal, and dropping it would leave a view
    // focused over a defunct process with the app looking frozen.
    void releaseDeferred(const QString &owner, std::function<void()> onRestored,
                         int delayMs = 200);

    // Synchronous restore with no callback, for shutdown paths. Cancels any
    // pending deferred release (discarding its callback, since the object that
    // would receive it is being torn down).
    void releaseNow(const QString &owner);

    // Second half of the restore, called once the restored window has actually
    // presented a frame: switches the VT back and clears the hand-off state.
    // main.cpp calls this from QQuickWindow::frameSwapped; a timer here covers the
    // case where no frame ever arrives so the Pi cannot be stranded on the
    // child's VT.
    void completeRestore();

signals:
    // Emitted once the hand-off is fully taken (VT switched, master dropped,
    // CRTC state saved). The owner of the QQuickWindow hides it here.
    //
    // This is not belt-and-braces. The VT switch is supposed to suspend Qt's
    // render thread through the kernel's VT_PROCESS signal, and on this Pi it
    // does NOT: the app holds no console fd (only /dev/dri/card1), so QFbVtHandler
    // was never installed and nothing tells EGLFS to stop drawing. The switch
    // still completes, so there is no warning and no failed-looking state — but
    // the renderer keeps submitting atomic commits to a DRM device it no longer
    // owns: ~500 EACCES lines per RetroArch launch, ~10% CPU for the whole
    // session, and a child that draws nothing would leave the stale UI on screen.
    // Hiding the window is the only lever that actually stops EGLFS rendering
    // from inside the app.
    void displayTaken();

    // Emitted after the display is put back (master restored, CRTC restored, VT
    // switched back). The window becomes visible again here.
    void displayReturned();

private:
    int  getActiveVt() const;
    // Never returns activeVt — switching to the VT we are already on is a silent
    // no-op that leaves Qt drawing with DRM master dropped. See the definition.
    int  findFreeVt(int activeVt) const;
    int  findQtDrmFd() const;
#ifdef Q_OS_LINUX
    // Our own console in preference to /dev/tty0 — see the comment on the
    // definition; using /dev/tty0 makes the switch BACK fail with EPERM.
    int  openVtControlFd() const;
#endif
    // False when the switch could not be performed (typically no permission on
    // /dev/tty0). The caller must know, because a hand-off without the VT switch
    // leaves Qt's renderer running while DRM master is dropped.
    bool switchToVt(int vt);
#ifdef Q_OS_LINUX
    void saveDrmCrtcState(int fd);
    void restoreDrmCrtcState(int fd);
    bool ensureBlankFb(int fd, uint32_t width, uint32_t height);
#endif
    void doRestore();

    // Tail of doRestore: brings the window back while the display is still on the
    // child's VT, waits for frames to settle, then switches back — so EGLFS's
    // re-init never happens in view. See the comment on the definition.
    void finishRestore();

    QString m_owner;                    // empty when not handed off
    int     m_previousVt   = -1;
    bool    m_switchedVt   = false;     // did the VT switch actually succeed?
    int     m_qtDrmFd      = -1;
    QTimer *m_releaseTimer = nullptr;
    QTimer *m_restoreTimer = nullptr;
    std::function<void()> m_onRestored;
#ifdef Q_OS_LINUX
    DrmSavedState m_savedDrm = {};
    //
    uint32_t m_blankFbId   = 0;
    uint32_t m_blankHandle = 0;
    uint32_t m_blankWidth  = 0;
    uint32_t m_blankHeight = 0;
#endif
};
