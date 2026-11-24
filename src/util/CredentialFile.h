#pragma once

// CFG-01 N4a: credential-file persistence.
//
// Thin, shape-aware wrapper over the shared atomic byte-writer (AtomicFile.h,
// lifted from CFG-01's ConfigFile.cpp). It adds the two invariants that are
// specific to RAW CREDENTIALS and deliberately absent from the generic writer:
//
//   1. 0600 on EVERY commit, including the first. QSaveFile preserves an
//      EXISTING target's mode, but a brand-new target is created with the
//      process umask (0644 on the Pi) — unacceptable for a token or a key.
//   2. Corrupt-preserving guard. An existing file that no longer parses as
//      its expected shape is evidence, not something to silently overwrite
//      with defaults. Automatic writers refuse (the user re-authenticates
//      explicitly, which removes the file first). An explicit user-supplied
//      value may pass allowCorrupt=true, which warns loudly but proceeds.
//
// On-disk formats/keys are UNCHANGED: indented JSON for the three auth files,
// compact JSON for YouTube auth, PEM for plex_key.pem, one UTF-8 line for the
// TVDB apikey. Durability is the shared writer's file-fsync-before-commit +
// dir-fsync-after-commit; every return value is checked there and here.

#include "AtomicFile.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QFileDevice>
#include <QJsonDocument>
#include <QString>

namespace credfile {

enum class Shape { JsonObject, ApiKeyLine, PemPrivateKey };

// Shape of an EXISTING live file. A missing file is not corruption (first
// write). Returns false + a human-readable detail when the bytes are damaged.
inline bool liveValid(const QString &path, Shape shape, QString *detail) {
    auto fail = [detail](const QString &why) {
        if (detail) *detail = why;
        return false;
    };
    QFile f(path);
    if (!f.exists()) return true;                       // first write
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("unreadable: %1").arg(f.errorString()));
    const QByteArray bytes = f.readAll();
    const bool readError = f.error() != QFile::NoError;
    f.close();
    if (readError)
        return fail(QStringLiteral("read error: %1").arg(f.errorString()));
    if (bytes.isEmpty())
        return fail(QStringLiteral("zero bytes"));
    if (bytes.contains('\0'))
        return fail(QStringLiteral("embedded NUL byte"));

    switch (shape) {
    case Shape::JsonObject: {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return fail(QStringLiteral("not a JSON object: %1")
                        .arg(err.errorString()));
        return true;
    }
    case Shape::ApiKeyLine: {
        const QByteArray line = bytes.split('\n').first().trimmed();
        if (line.isEmpty()) return fail(QStringLiteral("empty apikey line"));
        // A second non-blank line means a truncated/concatenated file.
        for (int i = 1; i < bytes.split('\n').size(); ++i) {
            if (!bytes.split('\n').at(i).trimmed().isEmpty())
                return fail(QStringLiteral("extra content after apikey line"));
        }
        return true;
    }
    case Shape::PemPrivateKey:
        if (!bytes.contains("-----BEGIN") || !bytes.contains("PRIVATE KEY-----"))
            return fail(QStringLiteral("missing PEM private-key marker"));
        return true;
    }
    return fail(QStringLiteral("unknown shape"));
}

// Atomically write credential bytes, enforce 0600, and guard pre-existing
// corruption. `forceFail` is the shared writer's runtime test seam
// (open/write/flush/fsync/commit/dirfsync); production callers pass nullptr.
// Returns false with the live file byte-identical on every pre-commit failure,
// on a corrupt-live refusal, or on a post-commit chmod failure (data is
// complete in that last case — only the mode may be loose).
inline bool write(const QString &path, const QByteArray &bytes, Shape shape,
                  const char *what, bool allowCorrupt = false,
                  const char *forceFail = nullptr) {
    QString detail;
    if (!liveValid(path, shape, &detail)) {
        if (!allowCorrupt) {
            qWarning("[credfile] REFUSING %s write at %s: live file is corrupt (%s) — sign out to clear it, then authenticate again",
                     what, qPrintable(path), qPrintable(detail));
            return false;
        }
        qWarning("[credfile] %s: overwriting corrupt live file at %s (%s) with an explicitly supplied value",
                 what, qPrintable(path), qPrintable(detail));
    }
    if (!atomicfile::writeBytes(path, bytes, /*durable=*/true, what, forceFail))
        return false;
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        qWarning("[credfile] %s: chmod 0600 failed for %s (data is complete, permissions may be loose)",
                 what, qPrintable(path));
        return false;
    }
    return true;
}

} // namespace credfile
