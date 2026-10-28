#include "RetroArchBackend.h"
#include "../../util/DisplayHandoff.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QProcess>
#include <QDebug>
#include <algorithm>

RetroArchBackend::RetroArchBackend(const QString &appRoot, const QString &dataRoot,
                                   DisplayHandoff *handoff, QObject *parent)
    : QObject(parent)
    , m_appRoot(appRoot)
    , m_dataRoot(dataRoot)
    , m_handoff(handoff) {
    loadSettings();
}

RetroArchBackend::~RetroArchBackend() = default;

void RetroArchBackend::loadSettings() {
    QFile f(m_dataRoot + "/config.json");
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject mod = doc.object()["modules"].toObject()["com.240mp.retroarch"].toObject();

            m_playlistDir = mod["playlist_directory"].toString();
            if (m_playlistDir.isEmpty())
                m_playlistDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                                 + "/retroarch/playlists";

            m_romDir = mod["rom_directory"].toString();
            if (m_romDir.isEmpty())
                m_romDir = QDir::homePath() + "/ROMs";

            m_thumbnailDir = mod["thumbnail_directory"].toString();
            if (m_thumbnailDir.isEmpty())
                m_thumbnailDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                                 + "/retroarch/thumbnails";

            m_retroarchPath = mod["retroarch_path"].toString();
            if (m_retroarchPath.isEmpty())
                m_retroarchPath = QStringLiteral("retroarch"); // from PATH
        }
    }
}

QString RetroArchBackend::playlist_directory() const { return m_playlistDir; }
QString RetroArchBackend::rom_directory() const { return m_romDir; }
QString RetroArchBackend::thumbnail_directory() const { return m_thumbnailDir; }
QString RetroArchBackend::retroarch_path() const { return m_retroarchPath; }

QStringList RetroArchBackend::playlistFilters() {
    // Modern RetroArch writes playlists as <core>.json; older builds wrote
    // .lpl. Scan both so either install works without configuration.
    return { QStringLiteral("*.lpl"), QStringLiteral("*.json") };
}

QString RetroArchBackend::systemFromPlaylist(const QString &playlistPath) const {
    return QFileInfo(playlistPath).baseName().toLower();
}

// ── Tier 1: systems ──────────────────────────────────────────────────────────

QVariantList RetroArchBackend::refreshSystems() {
    QVariantList systems;
    QDir playlistDir(m_playlistDir);

    if (!playlistDir.exists()) {
        emit systemsReady(systems);
        return systems;
    }

    QFileInfoList playlists =
        playlistDir.entryInfoList(playlistFilters(), QDir::Files | QDir::Readable, QDir::Name);

    // Each playlist file IS a system entry: name comes from the filename.
    // Count the games in it as its "weight" so a system with 400 games reads
    // as a different row than one with 2.
    for (const QFileInfo &pl : playlists) {
        const QString systemId = systemFromPlaylist(pl.filePath());
        if (systemId.isEmpty()) continue;

        QVariantList games = parsePlaylist(pl.filePath(), systemId);
        systems.append(QVariantMap{
            { "id", systemId },
            { "name", QFileInfo(pl.fileName()).baseName() },
            { "path", pl.filePath() },
            { "count", games.size() }
        });
    }

    std::sort(systems.begin(), systems.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["name"].toString() < b.toMap()["name"].toString();
    });

    emit systemsReady(systems);
    return systems;
}

// ── Tier 2: playlists of a system ────────────────────────────────────────────

QVariantList RetroArchBackend::refreshPlaylists(const QString &systemId) {
    QVariantList playlists;
    QDir playlistDir(m_playlistDir);

    if (!playlistDir.exists()) {
        emit playlistsReady(playlists);
        return playlists;
    }

    QFileInfoList plFiles =
        playlistDir.entryInfoList(playlistFilters(), QDir::Files | QDir::Readable, QDir::Name);

    for (const QFileInfo &pl : plFiles) {
        if (systemFromPlaylist(pl.filePath()) != systemId) continue;

        QVariantList games = parsePlaylist(pl.filePath(), systemId);
        playlists.append(QVariantMap{
            { "path", pl.filePath() },
            { "name", pl.baseName() },
            { "count", games.size() },
            { "systemId", systemId }
        });
    }

    std::sort(playlists.begin(), playlists.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["name"].toString() < b.toMap()["name"].toString();
    });

    emit playlistsReady(playlists);
    return playlists;
}

// ── Tier 3: games in a playlist ──────────────────────────────────────────────

QVariantList RetroArchBackend::refreshGames(const QString &playlistPath) {
    QVariantList games = parsePlaylist(playlistPath, systemFromPlaylist(playlistPath));
    emit gamesReady(games);
    return games;
}

QVariantList RetroArchBackend::parsePlaylist(const QString &path, const QString &systemId) {
    QVariantList entries;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return entries;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return entries;

    QJsonArray items = doc.object()["items"].toArray();
    for (const QJsonValue &val : items) {
        QJsonObject item = val.toObject();
        const QString label = item["label"].toString();
        const QString romPath = item["path"].toString();
        if (label.isEmpty() || romPath.isEmpty()) continue;

        entries.append(QVariantMap{
            { "name", label },
            { "romPath", resolveRomPath(path, romPath) },
            { "corePath", resolveCorePath(item["core_path"].toString()) },
            { "coreName", QFileInfo(item["core_path"].toString()).baseName() },
            { "dbName", item["db_name"].toString() },
            { "thumbnail", findThumbnail(label, systemId) },
            { "systemId", systemId }
        });
    }
    return entries;
}

QString RetroArchBackend::resolveRomPath(const QString &playlistPath,
                                         const QString &romPath) const {
    // Absolute paths are used as-is — the playlist is the source of truth.
    if (QFileInfo(romPath).isAbsolute()) return romPath;

    // Relative paths are stored relative to RetroArch's config dir (where the
    // playlists live), so try the playlist directory first…
    QFileInfo plInfo(playlistPath);
    QString candidate = plInfo.absoluteDir().filePath(romPath);
    if (QFileInfo(candidate).exists()) return candidate;

    // …then the configured ROM root (people commonly keep relative paths
    // rooted at their ROM folder).
    candidate = QDir(m_romDir).filePath(romPath);
    if (QFileInfo(candidate).exists()) return candidate;

    // Nothing on disk — return as-is so RetroArch still gets the path the
    // playlist recorded, and its own logging tells the user it's missing.
    return romPath;
}

QString RetroArchBackend::resolveCorePath(const QString &corePath) const {
    // Absolute: pass through. Relative filename: pass through too — RetroArch
    // resolves it against its own core directory (core_directory setting),
    // which is exactly where a user-installed core lives. Guessing a path
    // ourselves would break on any install layout we haven't anticipated.
    return corePath;
}

QString RetroArchBackend::findThumbnail(const QString &gameName,
                                        const QString &systemId) const {
    // RetroArch organises thumbnails as
    // <thumbnails>/<core_name>/<Type>/<name>.png, Type being one of
    // Named_Boxarts, Named_Snaps, Named_Titles, Boxarts, … — the "Named_"
    // variants match the label, which is what we key on.
    const QStringList thumbTypes = {
        QStringLiteral("Named_Boxarts"), QStringLiteral("Named_Snaps"),
        QStringLiteral("Named_Titles"),  QStringLiteral("Boxarts"),
        QStringLiteral("Snaps")
    };
    const QStringList extensions = { QStringLiteral(".png"), QStringLiteral(".jpg") };
    // entryInfoList takes GLOB patterns, not bare extensions — ".png" matches
    // nothing, which silently broke the case-insensitive fallback.
    const QStringList globs = { QStringLiteral("*.png"), QStringLiteral("*.jpg") };

    for (const QString &type : thumbTypes) {
        const QDir thumbDir(m_thumbnailDir + "/" + systemId + "/" + type);
        if (!thumbDir.exists()) continue;

        // Cheap exact checks first.
        for (const QString &ext : extensions) {
            const QString exact = thumbDir.filePath(gameName + ext);
            if (QFileInfo(exact).exists()) return exact;
        }

        // Case-insensitive match (labels and filenames disagree on case).
        const QFileInfoList files = thumbDir.entryInfoList(globs, QDir::Files);
        for (const QFileInfo &file : files) {
            if (file.completeBaseName().compare(gameName, Qt::CaseInsensitive) == 0)
                return file.absoluteFilePath();
        }
    }
    return QString();
}

// ── Launch ───────────────────────────────────────────────────────────────────

void RetroArchBackend::launchGame(const QVariantMap &game, const QString &systemId) {
    const QString romPath = game["romPath"].toString();
    const QString corePath = game["corePath"].toString();
    Q_UNUSED(systemId)

    if (romPath.isEmpty()) {
        qWarning("[RetroArch] No ROM path for game: %s", qPrintable(game["name"].toString()));
        return;
    }
    if (!QFileInfo(romPath).exists()) {
        qWarning("[RetroArch] ROM not found: %s", qPrintable(romPath));
        return;
    }

    QStringList args;
    if (!corePath.isEmpty())
        args << QStringLiteral("-L") << corePath;
    args << romPath;

    qInfo("[RetroArch] launching %s with args: %s",
          qPrintable(m_retroarchPath), qPrintable(args.join(QStringLiteral(" "))));

    // Same hand-off as mpv: acquire the display, let RetroArch take the
    // console, release (deferred — see DisplayHandoff's load-bearing ordering
    // comment) once the process exits.
    if (m_handoff && m_handoff->acquire(QLatin1String("retroarch")) < 0) {
        qWarning("[RetroArch] Cannot start: %s has the screen",
                 qPrintable(m_handoff->currentOwner()));
        return;
    }
    if (m_handoff && !m_handoff->savedStateValid()) {
        // A script-launcher can decline here; mpv deliberately cannot.
        // RetroArch is closer to the script case — we'd be stranding the
        // console with no way back if the restore can't restore.
        qWarning("[RetroArch] CRTC state not captured — refusing to launch "
                 "(restore could leave a black screen)");
        m_handoff->releaseNow(QLatin1String("retroarch"));
        return;
    }

    QProcess *process = new QProcess(this);
    process->setProgram(m_retroarchPath);
    process->setArguments(args);
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        Q_UNUSED(error)
        qWarning("[RetroArch] process error: %s", qPrintable(process->errorString()));
        if (m_handoff)
            m_handoff->releaseDeferred(QLatin1String("retroarch"), []() {});
        process->deleteLater();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        qInfo("[RetroArch] exited with code %d", exitCode);
        if (m_handoff)
            m_handoff->releaseDeferred(QLatin1String("retroarch"), []() {});
        process->deleteLater();
    });
    process->start();
}
