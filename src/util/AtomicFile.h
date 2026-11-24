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
// `forceFail` is a runtime test seam (production callers pass nullptr): one of
// "open", "write", "flush", "fsync", "commit", "dirfsync". Keeping it a
// runtime parameter — not an #ifdef — keeps this inline function ODR-identical
// across translation units.

#include <QByteArray>
#include <QDebug>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace atomicfile {

inline bool writeBytes(const QString &path, const QByteArray &bytes,
                       bool durable, const char *what,
                       const char *forceFail = nullptr) {
    const auto wants = [forceFail](const char *stage) {
        return forceFail && std::strcmp(forceFail, stage) == 0;
    };

    if (wants("open")) {
        qWarning("[atomicfile] %s: open failure injected for %s", what, qPrintable(path));
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("[atomicfile] %s: open failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    const qint64 written = f.write(bytes);
    if (written != bytes.size() || wants("write")) {
        qWarning("[atomicfile] %s: write failed for %s (%lld of %lld bytes): %s",
                 what, qPrintable(path), static_cast<long long>(written),
                 static_cast<long long>(bytes.size()), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (!f.flush() || wants("flush")) {
        qWarning("[atomicfile] %s: flush failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (durable) {
        // fsync the temp FILE before commit: rename gives old-or-new, not
        // durability (ext4 delayed allocation / SD volatile cache).
        const int fd = static_cast<int>(f.handle());
        if (wants("fsync") || fd < 0 || ::fsync(fd) != 0) {
            qWarning("[atomicfile] %s: file fsync failed for %s: %s",
                     what, qPrintable(path),
                     wants("fsync") ? "injected" : std::strerror(errno));
            f.cancelWriting();
            return false;
        }
    }
    if (wants("commit")) {
        qWarning("[atomicfile] %s: commit failure injected for %s", what, qPrintable(path));
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        qWarning("[atomicfile] %s: commit failed for %s: %s",
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
            qWarning("[atomicfile] %s: directory fsync failed for %s: %s "
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

} // namespace atomicfile
