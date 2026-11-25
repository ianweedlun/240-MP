#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>

// Shared read/write policy for small JSON state files (media-server auth,
// module config). Every caller gets the same four-way load distinction, the
// same quarantine behaviour, and the same owner-only atomic write.
//
// Header-only so backend TUs need no CMake source-list change.
namespace jsonstate {

enum class Status {
    Missing,    // no file at this path — a first run is legitimate
    Valid,      // parsed into a JSON object (the object itself may be empty)
    ParseError, // bytes present, not valid JSON — raw bytes quarantined
    NotObject,  // valid JSON, top level is not an object — raw bytes quarantined
    ReadError,  // file exists but could not be read — left untouched
};

inline QString statusText(Status status)
{
    switch (status) {
    case Status::Missing:    return QStringLiteral("missing");
    case Status::Valid:      return QStringLiteral("valid");
    case Status::ParseError: return QStringLiteral("parse-error");
    case Status::NotObject:  return QStringLiteral("not-object");
    case Status::ReadError:  return QStringLiteral("read-error");
    }
    return QStringLiteral("unknown");
}

struct LoadResult {
    Status status = Status::Missing;
    QJsonObject object;          // populated for Valid only
    QByteArray raw;              // exact bytes read, kept for quarantine and recovery
    QString quarantinePath;      // non-empty when raw bytes were preserved
    QString errorString;         // parse / I/O detail for a warning

    bool isCorrupt() const
    {
        return status == Status::ParseError || status == Status::NotObject;
    }
    bool quarantined() const { return !quarantinePath.isEmpty(); }

    // Routine (non-user-initiated) saves are safe only after a clean parse or
    // a genuinely missing file. Quarantined or unreadable state must never be
    // written back over: that is the silent-sign-out/clobber chain.
    bool allowsRoutineSave() const
    {
        return status == Status::Missing || status == Status::Valid;
    }

    // A valid but empty object is clean JSON, yet still carries no state to
    // merge. Config callers that treat {} as unusable can refuse it explicitly.
    bool isEmptyObject() const
    {
        return status == Status::Valid && object.isEmpty();
    }
};

inline QString quarantinePathFor(const QString &path)
{
    return path + QStringLiteral(".corrupt");
}

// Qt's QFileDevice::Permission values are not POSIX octal bits, and the owner
// bits are reported under both Owner and User aliases. Map to canonical bits
// before comparing a committed file's mode with what we asked for.
inline int permissionBits(QFileDevice::Permissions perms)
{
    int bits = 0;
    if (perms & (QFileDevice::ReadOwner | QFileDevice::ReadUser))   bits |= 0400;
    if (perms & (QFileDevice::WriteOwner | QFileDevice::WriteUser)) bits |= 0200;
    if (perms & (QFileDevice::ExeOwner | QFileDevice::ExeUser))     bits |= 0100;
    if (perms & QFileDevice::ReadGroup)                             bits |= 0040;
    if (perms & QFileDevice::WriteGroup)                            bits |= 0020;
    if (perms & QFileDevice::ExeGroup)                              bits |= 0010;
    if (perms & QFileDevice::ReadOther)                             bits |= 0004;
    if (perms & QFileDevice::WriteOther)                            bits |= 0002;
    if (perms & QFileDevice::ExeOther)                              bits |= 0001;
    return bits;
}

// Keep group/world bits off the written file. An existing owner-only mode is
// reused (with owner read/write ensured so the next save can open the file);
// anything wider is reduced to 0600. QSaveFile alone is not enough here: on
// POSIX a newly committed file otherwise inherits the process umask.
inline QFileDevice::Permissions ownerOnlyPermissions(const QString &path)
{
    const QFileDevice::Permissions groupWorld =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QFileDevice::Permissions existing = QFile::permissions(path);
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if ((existing & groupWorld) != QFileDevice::Permissions{})
        return owner;
    return existing | owner;
}

// Preserve corrupt bytes before any rewrite. The primary quarantine is
// <path>.corrupt; if one already exists it is left intact and the new bytes
// get a timestamped sibling, so an earlier incident is never overwritten.
inline QString quarantineRaw(const QString &path, const QByteArray &raw)
{
    const QString primary = quarantinePathFor(path);
    if (!QFileInfo::exists(primary) && QFile::rename(path, primary))
        return primary; // rename keeps the bytes and their original mode

    QString target = primary;
    if (QFileInfo::exists(target)) {
        target = primary + QLatin1Char('.') +
                 QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz"));
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return {};
    if (!out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        out.write(raw) != raw.size() || !out.commit())
        return {};
    QFile::remove(path); // quarantine is durable; drop the corrupt live file
    return target;
}

// Load one JSON object state file. ParseError and NotObject quarantine the
// raw bytes before returning, so callers cannot accidentally rewrite first.
inline LoadResult load(const QString &path)
{
    LoadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(path)) {
            result.status = Status::Missing;
            return result;
        }
        result.status = Status::ReadError;
        result.errorString = file.errorString();
        return result;
    }
    result.raw = file.readAll();
    const QFileDevice::FileError readError = file.error();
    file.close();
    if (readError != QFileDevice::NoError) {
        result.status = Status::ReadError;
        result.errorString = file.errorString();
        result.raw.clear();
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(result.raw, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.status = Status::ParseError;
        result.errorString = parseError.errorString();
        result.quarantinePath = quarantineRaw(path, result.raw);
        return result;
    }
    if (!doc.isObject()) {
        result.status = Status::NotObject;
        result.errorString = QStringLiteral("top-level JSON value is not an object");
        result.quarantinePath = quarantineRaw(path, result.raw);
        return result;
    }
    result.status = Status::Valid;
    result.object = doc.object();
    return result;
}

enum class SaveMode {
    Routine,  // refuse when a .corrupt quarantine marker is present
    Explicit, // user-initiated re-auth/reset may replace a quarantined file
};

struct SaveResult {
    bool ok = false;
    QString errorString;
    bool refusedByQuarantine = false;
};

// Atomic save: QSaveFile + commit(), never a truncate-in-place QFile write.
// Every open/write/commit/permission step is checked; on failure the previous
// file is untouched. On POSIX the final mode is verified after commit.
inline SaveResult save(const QString &path, const QJsonObject &object,
                       SaveMode mode = SaveMode::Routine)
{
    SaveResult result;
    if (mode == SaveMode::Routine && QFileInfo::exists(quarantinePathFor(path))) {
        result.errorString = QStringLiteral("quarantined state present; routine save refused");
        result.refusedByQuarantine = true;
        return result;
    }

    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    const QFileDevice::Permissions perms = ownerOnlyPermissions(path);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.errorString = file.errorString();
        return result;
    }
    if (!file.setPermissions(perms)) {
        result.errorString = file.errorString();
        file.cancelWriting();
        return result;
    }
    if (file.write(bytes) != bytes.size()) {
        result.errorString = file.errorString();
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.errorString = file.errorString();
        return result;
    }
#ifdef Q_OS_UNIX
    const int wantedBits = permissionBits(perms);
    const int committedBits = permissionBits(QFile::permissions(path));
    if (committedBits != wantedBits) {
        QFile::setPermissions(path, perms);
        const int finalBits = permissionBits(QFile::permissions(path));
        if (finalBits != wantedBits) {
            result.errorString =
                QStringLiteral("owner-only permission verification failed (wanted %1, got %2)")
                    .arg(QString::number(wantedBits, 8), QString::number(finalBits, 8));
            return result;
        }
    }
#endif
    result.ok = true;
    return result;
}

// Best-effort scalar recovery from quarantined (possibly truncated) bytes, so
// a caller can keep a device id instead of minting a new identity. Handles
// unescaped string values only — the common UUID/token shape, not JSON blobs.
inline QString recoverStringField(const QByteArray &raw, const QString &key)
{
    const QString text = QString::fromUtf8(raw);
    const QRegularExpression pattern(
        QStringLiteral("\"%1\"\\s*:\\s*\"([^\"\\\\]*)\"").arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString{};
}

} // namespace jsonstate
