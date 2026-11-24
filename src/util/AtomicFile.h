#pragma once

// Shared atomic byte-writer for 240-MP's data-dir persistence files.
//
// This is the generic shape factored out of CFG-01's src/util/ConfigFile.cpp
// (t_038efa5a): QSaveFile writes a temp in the TARGET'S OWN directory, every
// return value is checked, the file is fsynced BEFORE commit, the containing
// directory is fsynced AFTER commit, and every failure logs a qWarning and
// returns false. A pre-commit failure leaves the live target byte-identical;
// the uncommitted temp is discarded by QSaveFile. A post-commit dir-fsync
// failure returns false with the live file already holding the NEW COMPLETE
// document — never a wipe.
//
// Header-only on purpose: several test targets compile a single backend .cpp
// without ConfigFile.cpp, so a compiled .cpp here would break their links.
// It is also the canonical implementation ConfigFile.cpp delegates to, so
// there is exactly one copy of the shape in the tree.
//
// `durable=false` skips both fsyncs for regenerable caches; every in-scope
// persistence file in the N4c rollout uses the default `durable=true`.
// Warnings keep the `[cfgfile]` tag from ConfigFile.cpp's original atomicWrite
// — the CFG-01 G3 kill-sweep gate matches on it, so changing the tag breaks
// a checked-in gate even though the data-dir callers are not config.json.
// `forceFail` is a runtime test seam (production callers pass nullptr): one of
// "open", "write", "flush", "fsync", "commit", "dirfsync". Keeping it a
// runtime parameter — not an #ifdef — keeps this inline function ODR-identical
// across translation units.

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace atomicfile {

namespace detail {

inline bool wantsStage(const char *forceFail, const char *stage)
{
    return forceFail && std::strcmp(forceFail, stage) == 0;
}

// fsync the directory containing `path` so a rename/link is durable. The live
// file is already the new complete document by the time this runs; a failure
// here is reported but never wipes.
inline bool fsyncDir(const QString &path, const char *what, bool injected)
{
    const QString dirPath = QFileInfo(path).absolutePath();
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int dfd = ::open(QFile::encodeName(dirPath).constData(), flags);
    const bool failed = injected || dfd < 0 || ::fsync(dfd) != 0;
    if (failed) {
        qWarning("[cfgfile] %s: directory fsync failed for %s: %s "
                 "(live file is the new complete document)",
                 what, qPrintable(dirPath),
                 injected ? "injected" : std::strerror(errno));
    }
    if (dfd >= 0) ::close(dfd);
    return !failed;
}

} // namespace detail

inline bool writeBytes(const QString &path, const QByteArray &bytes,
                       bool durable, const char *what,
                       const char *forceFail = nullptr) {
    const auto wants = [forceFail](const char *stage) {
        return forceFail && std::strcmp(forceFail, stage) == 0;
    };

    if (wants("open")) {
        qWarning("[cfgfile] %s: open failure injected for %s", what, qPrintable(path));
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("[cfgfile] %s: open failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    const qint64 written = f.write(bytes);
    if (written != bytes.size() || wants("write")) {
        qWarning("[cfgfile] %s: write failed for %s (%lld of %lld bytes): %s",
                 what, qPrintable(path), static_cast<long long>(written),
                 static_cast<long long>(bytes.size()), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (!f.flush() || wants("flush")) {
        qWarning("[cfgfile] %s: flush failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (durable) {
        // fsync the temp FILE before commit: rename gives old-or-new, not
        // durability (ext4 delayed allocation / SD volatile cache).
        const int fd = static_cast<int>(f.handle());
        if (wants("fsync") || fd < 0 || ::fsync(fd) != 0) {
            qWarning("[cfgfile] %s: file fsync failed for %s: %s",
                     what, qPrintable(path),
                     wants("fsync") ? "injected" : std::strerror(errno));
            f.cancelWriting();
            return false;
        }
    }
    if (wants("commit")) {
        qWarning("[cfgfile] %s: commit failure injected for %s", what, qPrintable(path));
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        qWarning("[cfgfile] %s: commit failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        return false;   // QSaveFile discards the uncommitted temp
    }
    if (durable) {
        // fsync the CONTAINING DIRECTORY so the rename itself is durable.
        const QString dirPath = QFileInfo(path).absolutePath();
        int flags = O_RDONLY;
#ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#endif
        const int dfd = ::open(QFile::encodeName(dirPath).constData(), flags);
        const bool failed = wants("dirfsync") || dfd < 0 || ::fsync(dfd) != 0;
        if (failed) {
            qWarning("[cfgfile] %s: directory fsync failed for %s: %s "
                     "(live file is the new complete document)",
                     what, qPrintable(dirPath),
                     wants("dirfsync") ? "injected" : std::strerror(errno));
            if (dfd >= 0) ::close(dfd);
            return false;
        }
        ::close(dfd);
    }
    return true;
}

// Atomic CREATE with a no-clobber contract: a stub file must never overwrite
// an existing (possibly hand-edited) target, even if that target appears in
// the tiny window between the caller's exists() guard and the publish step.
//
// QSaveFile::commit() is a rename — it would clobber. Instead we fully write
// and fsync a uniquely named temp in the target's own directory, then publish
// with link(2): link() fails atomically with EEXIST if the target appeared,
// and a crash before link leaves only the temp (target absent), never a
// partial target. The temp is unlinked after publishing. This preserves BOTH
// old-or-new-complete atomicity AND the O_EXCL no-clobber guarantee that the
// old QFile(WriteOnly|NewOnly) call sites had.
//
// Requires a filesystem with hardlink support (the app's ext4 data dir and
// macOS APFS both qualify). On an exotic FS where link() is unsupported the
// write fails loudly and the caller keeps its defaults — never a silent
// clobber. `forceFail` stages: open|write|flush|fsync|clobber|dirfsync.
inline bool writeBytesNewOnly(const QString &path, const QByteArray &bytes,
                              bool durable, const char *what,
                              const char *forceFail = nullptr) {
    const auto wants = [forceFail](const char *stage) {
        return detail::wantsStage(forceFail, stage);
    };

    if (wants("open")) {
        qWarning("[cfgfile] %s: open failure injected for %s", what, qPrintable(path));
        return false;
    }
    const QString tmp = path + QStringLiteral(".newonly-%1-%2")
                            .arg(QCoreApplication::applicationPid())
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        qWarning("[cfgfile] %s: temp open failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    const qint64 written = f.write(bytes);
    if (written != bytes.size() || wants("write")) {
        qWarning("[cfgfile] %s: write failed for %s (%lld of %lld bytes): %s",
                 what, qPrintable(path), static_cast<long long>(written),
                 static_cast<long long>(bytes.size()), qPrintable(f.errorString()));
        f.remove();
        return false;
    }
    if (!f.flush() || wants("flush")) {
        qWarning("[cfgfile] %s: flush failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        f.remove();
        return false;
    }
    if (durable) {
        const int fd = static_cast<int>(f.handle());
        if (wants("fsync") || fd < 0 || ::fsync(fd) != 0) {
            qWarning("[cfgfile] %s: file fsync failed for %s: %s",
                     what, qPrintable(path),
                     wants("fsync") ? "injected" : std::strerror(errno));
            f.remove();
            return false;
        }
    }
    f.close();

    // Publish atomically with no-clobber. wants("clobber") simulates a racing
    // creator having won: behave as if link() returned EEXIST and leave any
    // existing target untouched.
    const bool clobber = wants("clobber")
            || ::link(QFile::encodeName(tmp).constData(),
                      QFile::encodeName(path).constData()) != 0;
    if (clobber) {
        qWarning("[cfgfile] %s: target already exists (no-clobber) for %s",
                 what, qPrintable(path));
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(tmp);   // the link is now the sole name for the inode
    if (durable && !detail::fsyncDir(path, what, wants("dirfsync")))
        return false;
    return true;
}

} // namespace atomicfile
