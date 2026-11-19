#pragma once
// Per-instance temp paths for the mpv player (T2, issue #111).
//
// MpvController used four process-global fixed paths under QDir::tempPath();
// two concurrent 240-MP instances (or lab harnesses) collided on the shared
// IPC socket — mpv unlinks+rebinds an existing path, so one instance's mpv
// stole the other's socket. Every path now carries an instance token
// "<pid>-<seq>": the pid separates processes, the per-process construction
// sequence separates multiple controllers inside one process (test rigs).
//
// HARD CONSTRAINT: the IPC socket is a sockaddr_un path — sun_path holds
// 108 bytes on Linux (107 usable incl. the NUL), 104 on macOS. QDir::tempPath()
// honors $TMPDIR and can be arbitrarily long. When the socket path would
// exceed the platform limit, IPC is DISABLED for that instance (ipcUsable
// false, socket empty) — never relocated to a shorter directory: a silent
// relocation breaks the discovery contract (the launch log publishes the
// path) and the RG-3 regression arm that deliberately pads TMPDIR past the
// limit. Playback continues without IPC; quit goes through forceStop.
// The log/input-conf/subinfo files are regular files — no sun_path bound.
//
// Pure logic, no QObject — unit-tested by mpv-instance-paths-tests.

#include <QString>
#include <QtGlobal>

#ifndef Q_OS_WIN
#include <sys/un.h>
#endif

namespace mpvipc {

// Usable sun_path bytes for a socket path (excluding the terminating NUL).
// Platform-correct by construction: Linux 107, macOS 103. Windows has no
// sockaddr_un (QLocalSocket uses named pipes) — no bound there.
constexpr int sunPathLimit()
{
#ifdef Q_OS_WIN
    return 4096;   // named-pipe name budget; effectively unbounded here
#else
    return static_cast<int>(sizeof(sockaddr_un::sun_path)) - 1;
#endif
}

struct InstancePaths {
    QString socket;       // empty when ipcUsable is false
    QString log;          // mpv --log-file (regular file, always set)
    QString inputConf;    // mpv --input-conf (regular file, always set)
    QString subInfo;      // OSC subinfo-file JSON (regular file, always set)
    bool    ipcUsable = true;
    QString rejectedSocket;   // the over-limit path, for the warning log
    int     rejectedLen = 0;  // its UTF-8 byte length
};

// Build the four per-instance paths. tempDir is QDir::tempPath(); pid the
// app process id; seq a per-process construction counter starting at 1.
// The socket is kept only when its UTF-8 byte length fits sunPathLimit().
inline InstancePaths makeInstancePaths(const QString &tempDir, qint64 pid, int seq)
{
    InstancePaths p;
    const QString token = QStringLiteral("%1-%2").arg(pid).arg(seq);
    const QString base  = tempDir + QStringLiteral("/240mp-");
    p.log       = base + QStringLiteral("mpv-%1.log").arg(token);
    p.inputConf = base + QStringLiteral("input-%1.conf").arg(token);
    p.subInfo   = base + QStringLiteral("mpv-%1.subinfo.json").arg(token);

    const QString sock = base + QStringLiteral("mpv-%1.sock").arg(token);
    const int len = sock.toUtf8().size();
    if (len <= sunPathLimit()) {
        p.socket = sock;
    } else {
        p.ipcUsable = false;
        p.rejectedSocket = sock;
        p.rejectedLen = len;
    }
    return p;
}

} // namespace mpvipc
