#include "ConfigFile.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace cfgfile {

QJsonObject defaultDocument() {
    return QJsonObject{
        {"app", QJsonObject{{"color_scheme", "Video 1"}}},
        {"modules", QJsonObject{}}
    };
}

bool passesGuard(const QJsonObject &doc) {
    const bool hasApp = doc.contains(QStringLiteral("app"));
    const bool hasModules = doc.contains(QStringLiteral("modules"));
    if (!hasApp && !hasModules)
        return false;   // {} — the stale-empty-doc incident shape
    if (hasApp && !doc.value(QStringLiteral("app")).isObject())
        return false;
    if (hasModules && !doc.value(QStringLiteral("modules")).isObject())
        return false;
    return true;
}

namespace {

QString configPath(const QString &dataRoot) {
    return dataRoot + QStringLiteral("/config.json");
}
QString bakPath(const QString &dataRoot) {
    return dataRoot + QStringLiteral("/config.json.bak");
}

// Test-only failure injection, compile-gated OFF in every production target
// (G16c). The cfg01 Tier-2 test target defines CFGFILE_TEST_SEAM; nothing else
// does. Env: CFG01_SEAM_FAIL=open|write|flush|fsync|commit|dirfsync.
bool seamWants(const char *stage) {
#ifdef CFGFILE_TEST_SEAM
    return qEnvironmentVariable("CFG01_SEAM_FAIL") == QLatin1String(stage);
#else
    Q_UNUSED(stage);
    return false;
#endif
}

// Atomic write of raw bytes to `path` via QSaveFile (temp in the SAME
// directory). `durable` = file-fsync before commit + dir-fsync after commit
// (REQUIRED for config.json; siblings are best-effort). Every return checked;
// any pre-commit failure leaves the live target byte-identical and discards
// the temp. Dir-fsync failure (post-commit) returns false with the live file
// already the NEW COMPLETE doc — never a wipe (R6).
bool atomicWrite(const QString &path, const QByteArray &bytes, bool durable,
                 const char *what) {
    if (seamWants("open")) {
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
    if (written != bytes.size() || seamWants("write")) {
        qWarning("[cfgfile] %s: write failed for %s (%lld of %lld bytes): %s",
                 what, qPrintable(path), static_cast<long long>(written),
                 static_cast<long long>(bytes.size()), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (!f.flush() || seamWants("flush")) {
        qWarning("[cfgfile] %s: flush failed for %s: %s",
                 what, qPrintable(path), qPrintable(f.errorString()));
        f.cancelWriting();
        return false;
    }
    if (durable) {
        // fsync the temp FILE before commit: rename gives old-or-new, not
        // durability (ext4 delayed allocation / SD volatile cache).
        const int fd = static_cast<int>(f.handle());
        if (seamWants("fsync") || fd < 0 || ::fsync(fd) != 0) {
            qWarning("[cfgfile] %s: file fsync failed for %s: %s",
                     what, qPrintable(path),
                     seamWants("fsync") ? "injected" : strerror(errno));
            f.cancelWriting();
            return false;
        }
    }
    if (seamWants("commit")) {
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
        const bool failed = seamWants("dirfsync") || dfd < 0 || ::fsync(dfd) != 0;
        if (failed) {
            qWarning("[cfgfile] %s: directory fsync failed for %s: %s (live file is the new complete document)",
                     what, qPrintable(dirPath),
                     seamWants("dirfsync") ? "injected" : strerror(errno));
            if (dfd >= 0) ::close(dfd);
            return false;
        }
        ::close(dfd);
    }
    return true;
}

// Parse + structural guard. Returns false with a human-readable reason.
bool parseAndGuard(const QByteArray &bytes, QJsonObject &doc, QString &detail) {
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        detail = QStringLiteral("parse error at offset %1: %2")
                     .arg(err.offset).arg(err.errorString());
        return false;
    }
    if (!d.isObject()) {
        detail = QStringLiteral("root is not a JSON object");
        return false;
    }
    doc = d.object();
    if (!passesGuard(doc)) {
        detail = QStringLiteral("structural guard failed (need object root with at least one of app/modules, each an object)");
        return false;
    }
    return true;
}

// Quarantine = COPY of the corrupt bytes (never a move of the live file in the
// no-bak case: restart re-detection depends on the live file staying corrupt).
// Naming: config.json.corrupt-<UTC yyyyMMddTHHmmssZ>, collision -> .N suffix.
// Retention: newest 3 kept, older pruned. Dedup: identical bytes already in
// the newest quarantine -> no new copy (per-read spam would churn the SD card;
// the <=3 retention invariant is the contract, not the frequency).
bool quarantineCorrupt(const QString &dataRoot, const QByteArray &corruptBytes,
                       QString &detail) {
    QDir dir(dataRoot);
    const QStringList filters{QStringLiteral("config.json.corrupt-*")};
    QStringList existing = dir.entryList(filters, QDir::Files, QDir::Name);
    if (!existing.isEmpty()) {
        QFile newest(dir.filePath(existing.last()));
        if (newest.open(QIODevice::ReadOnly) && newest.readAll() == corruptBytes)
            return true;   // this exact corruption is already quarantined
    }
    const QString ts = QDateTime::currentDateTimeUtc()
                           .toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const QString base = QStringLiteral("config.json.corrupt-") + ts;
    QString name = base;
    for (int n = 1; dir.exists(name); ++n)
        name = base + QStringLiteral(".%1").arg(n);
    if (!atomicWrite(dir.filePath(name), corruptBytes, /*durable=*/false,
                     "config.json quarantine")) {
        detail = QStringLiteral("quarantine copy failed for %1").arg(name);
        return false;
    }
    // Retention: keep the newest 3 (name order == chronological).
    existing = dir.entryList(filters, QDir::Files, QDir::Name);
    while (existing.size() > 3) {
        const QString oldest = existing.takeFirst();
        if (!dir.remove(oldest))
            qWarning("[cfgfile] quarantine retention: could not prune %s",
                     qPrintable(oldest));
    }
    return true;
}

} // namespace

LoadResult load(const QString &dataRoot) {
    const QString path = configPath(dataRoot);
    QFile f(path);
    if (!f.exists())
        return {defaultDocument(), Status::Missing, {}};

    if (!f.open(QIODevice::ReadOnly)) {
        const QString detail = QStringLiteral("I/O error: %1").arg(f.errorString());
        qWarning("[cfgfile] config.json unreadable at %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return {defaultDocument(), Status::Corrupt, detail};
    }
    const QByteArray bytes = f.readAll();
    const bool readError = f.error() != QFile::NoError;
    const QString ioError = f.errorString();
    f.close();

    QJsonObject doc;
    QString detail;
    if (!readError && parseAndGuard(bytes, doc, detail))
        return {doc, Status::Ok, {}};
    if (readError)
        detail = QStringLiteral("I/O error during read: %1").arg(ioError);

    // CORRUPT — loud, distinguishable from Missing (R12).
    qWarning("[cfgfile] config.json corrupt at %s: %s",
             qPrintable(path), qPrintable(detail));

    // With a valid backup: quarantine the corrupt bytes, restore atomically.
    QFile bf(bakPath(dataRoot));
    if (bf.exists() && bf.open(QIODevice::ReadOnly)) {
        const QByteArray bakBytes = bf.readAll();
        const bool bakReadOk = bf.error() == QFile::NoError;
        bf.close();
        QJsonObject bakDoc;
        QString bakDetail;
        if (bakReadOk && parseAndGuard(bakBytes, bakDoc, bakDetail)) {
            QString qDetail;
            if (quarantineCorrupt(dataRoot, bytes, qDetail)
                && atomicWrite(path, bakBytes, /*durable=*/true, "config.json restore")) {
                qInfo("[cfgfile] config.json recovered from backup %s",
                      qPrintable(bakPath(dataRoot)));
                return {bakDoc, Status::RecoveredFromBackup, detail};
            }
            qWarning("[cfgfile] config.json recovery from backup failed (%s) — degraded",
                     qPrintable(qDetail.isEmpty() ? QStringLiteral("restore write failed") : qDetail));
            return {defaultDocument(), Status::Corrupt, detail};
        }
    }

    // No usable backup: quarantine a COPY and leave the live corrupt file in
    // place byte-identical — it is the evidence and the re-detection marker.
    QString qDetail;
    if (!quarantineCorrupt(dataRoot, bytes, qDetail))
        qWarning("[cfgfile] %s", qPrintable(qDetail));
    return {defaultDocument(), Status::Corrupt, detail};
}

bool save(const QString &dataRoot, const QJsonObject &doc) {
    const QString path = configPath(dataRoot);

    // INTRINSIC writer guard (R8/R17 — the P2 lesson): re-validate the LIVE
    // file before writing, regardless of caller or doc content. A future sixth
    // writer gets this for free. Only reset() may write over a corrupt file.
    QFile live(path);
    const bool priorExists = live.exists();
    QByteArray priorBytes;
    bool priorValid = false;
    if (priorExists) {
        if (live.open(QIODevice::ReadOnly)) {
            priorBytes = live.readAll();
            QJsonObject priorDoc;
            QString detail;
            priorValid = live.error() == QFile::NoError
                         && parseAndGuard(priorBytes, priorDoc, detail);
            live.close();
        }
        if (!priorValid) {
            // One-shot per data dir per process: degraded Settings keypress
            // storms must not spam the journal (the corrupt LOAD already warns
            // every time), but each distinct dir gets its own first warning.
            static QSet<QString> warnedDirs;
            if (!warnedDirs.contains(path)) {
                qWarning("[cfgfile] REFUSING config.json write at %s: live file is corrupt or unreadable — settings are protected until a confirmed reset",
                         qPrintable(path));
                warnedDirs.insert(path);
            }
            return false;
        }
    }

    const QByteArray payload = QJsonDocument(doc).toJson(QJsonDocument::Indented);
    if (!atomicWrite(path, payload, /*durable=*/true, "config.json"))
        return false;

    // Bak maintenance (R7): preserve the PREVIOUS live bytes byte-identical,
    // atomically, best-effort. First-ever save creates no bak. A bak failure
    // degrades recovery, never causes a wipe — warn, do not block.
    if (priorExists && priorValid
        && !atomicWrite(bakPath(dataRoot), priorBytes, /*durable=*/false, "config.json.bak")) {
        qWarning("[cfgfile] config.json.bak maintenance failed (non-blocking) for %s",
                 qPrintable(dataRoot));
    }
    return true;
}

bool reset(const QString &dataRoot, const QJsonObject &freshDefaults) {
    const QString path = configPath(dataRoot);
    QFile live(path);
    if (!live.exists())
        return false;   // nothing damaged to reset
    if (!live.open(QIODevice::ReadOnly)) {
        qWarning("[cfgfile] reset refused: cannot read live config at %s: %s",
                 qPrintable(path), qPrintable(live.errorString()));
        return false;   // cannot quarantine bytes we cannot read — fail closed
    }
    const QByteArray bytes = live.readAll();
    const bool readError = live.error() != QFile::NoError;
    live.close();
    QJsonObject doc;
    QString detail;
    if (!readError && parseAndGuard(bytes, doc, detail))
        return false;   // HEALTHY — never clobber a good file (guard-rail)

    QString qDetail;
    if (!quarantineCorrupt(dataRoot, bytes, qDetail)) {
        qWarning("[cfgfile] reset refused: %s", qPrintable(qDetail));
        return false;   // degraded persists
    }
    if (!atomicWrite(path, QJsonDocument(freshDefaults).toJson(QJsonDocument::Indented),
                     /*durable=*/true, "config.json reset")) {
        return false;
    }
    qInfo("[cfgfile] config.json reset to defaults; damaged bytes quarantined in %s",
          qPrintable(dataRoot));
    return true;
}

} // namespace cfgfile