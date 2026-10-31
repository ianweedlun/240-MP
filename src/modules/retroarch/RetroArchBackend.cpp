#include "RetroArchBackend.h"
#include "../../util/DisplayHandoff.h"
#include <QBuffer>
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
            QJsonObject root = doc.object();
            QJsonObject mod = root["modules"].toObject()["com.240mp.retroarch"].toObject();

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

            m_playlistSort = mod["playlist_sort"].toString();
            if (m_playlistSort.isEmpty())
                m_playlistSort = QStringLiteral("SYSTEM");

            // ── Migrate old show_preview (toggle) → thumbnail_display (list_single) ──
            bool needsSave = false;
            QString thumbDisplay;
            if (!mod.contains("thumbnail_display") || mod["thumbnail_display"].isNull()) {
                if (mod.contains("show_preview")) {
                    bool oldPreview = mod["show_preview"].toBool(true);
                    thumbDisplay = oldPreview ? "RIGHT" : "OFF";
                    needsSave = true;
                } else {
                    thumbDisplay = "RIGHT"; // default
                    needsSave = true;
                }
                // Remove the old key
                mod.remove("show_preview");
            }
            if (needsSave) {
                qInfo("[RetroArch] Migrating show_preview -> thumbnail_display: %s", qPrintable(thumbDisplay));
                mod.insert("thumbnail_display", QJsonValue::fromVariant(thumbDisplay));
                // Write back the modified module object — must reassign the whole
                // modules object since QJsonObject is implicitly shared.
                QJsonObject modules = root["modules"].toObject();
                modules.insert("com.240mp.retroarch", mod);
                root["modules"] = modules;
                // Close the read handle first, then write with a fresh QFile.
                f.close();
                QFile fw(m_dataRoot + "/config.json");
                if (fw.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    fw.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                    qInfo("[RetroArch] Config migration written to %s", qPrintable(fw.fileName()));
                } else {
                    qWarning("[RetroArch] Failed to write migrated config: %s", qPrintable(fw.errorString()));
                }
            }
        }
    }
}

QString RetroArchBackend::playlistSort() const { return m_playlistSort; }

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

QString RetroArchBackend::shortNameForSystem(const QString &systemId) const {
    // Short display names keyed on the playlist system id, which is the
    // playlist filename without extension, lowercased. This RetroArch install
    // uses human-named playlists ("Sega - Mega Drive - Genesis.lpl" ->
    // "sega - mega drive - genesis"), so the keys here match that form.
    static const QHash<QString, QString> shortNames = {
        { "atari - 2600", "ATARI 2600" },
        { "atari - 5200", "ATARI 5200" },
        { "atari - 7800", "ATARI 7800" },
        { "atari - lynx", "ATARI LYNX" },
        { "coleco - colecovision", "COLECOVISION" },
        { "fbneo - arcade games", "ARCADE" },
        { "nec - pc engine supergrafx", "PC ENGINE SGFX" },
        { "nec - pc engine - turbografx 16", "TG-16" },
        { "nintendo - game boy advance", "GBA" },
        { "nintendo - game boy color", "GBC" },
        { "nintendo - game boy", "GB" },
        { "nintendo - nintendo entertainment system", "NES" },
        { "nintendo - super nintendo entertainment system", "SNES" },
        { "sega - 32x", "32X" },
        { "sega - dreamcast", "DREAMCAST" },
        { "sega - game gear", "GAME GEAR" },
        { "sega - master system - mark iii", "MASTER SYSTEM" },
        { "sega - mega drive - genesis", "GENESIS" },
        { "snk - neo geo pocket color", "NGP COLOR" },
        { "sony - playstation", "PSX" },
    };

    if (shortNames.contains(systemId)) {
        return shortNames[systemId];
    }

    // Fallback for a playlist we don't have a curated short name for: drop the
    // manufacturer prefix ("Sega - X" -> "X"). Returning the (possibly long)
    // system descriptor beats inventing a wrong code, and the QML layer only
    // shows the prefix when it actually differs from the name, so this never
    // renders a duplicated "[Foo - Bar] Foo - Bar" row.
    const QString sep = QStringLiteral(" - ");
    const int at = systemId.indexOf(sep);
    if (at != -1) {
        const QString system = systemId.mid(at + sep.size());
        if (!system.isEmpty())
            return system.toUpper();
    }
    return systemId.toUpper();
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

QVariantList RetroArchBackend::refreshAllPlaylists() {
    QVariantList playlists;
    QDir playlistDir(m_playlistDir);

    if (!playlistDir.exists()) {
        return playlists;
    }

    QFileInfoList plFiles =
        playlistDir.entryInfoList(playlistFilters(), QDir::Files | QDir::Readable, QDir::Name);

    for (const QFileInfo &pl : plFiles) {
        const QString systemId = systemFromPlaylist(pl.filePath());
        if (systemId.isEmpty()) continue;

        QVariantList games = parsePlaylist(pl.filePath(), systemId);
        playlists.append(QVariantMap{
            { "path", pl.filePath() },
            { "name", pl.baseName() },
            { "count", games.size() },
            { "systemId", systemId },
            { "shortName", shortNameForSystem(systemId) }
        });
    }

    // Order playlists according to playlist_sort setting:
    // - SYSTEM (default): short display name ("GENESIS", "NES", "SNES")
    // - ALPHA: raw playlist basename ("Nintendo - Super Nintendo...", "Sega - Mega Drive...")
    // - COMPANY: manufacturer prefix (text before first " - "), fallback to alpha
    const QString sortMode = m_playlistSort;
    std::sort(playlists.begin(), playlists.end(), [sortMode](const QVariant &a, const QVariant &b) {
        const auto ma = a.toMap();
        const auto mb = b.toMap();
        if (sortMode == QStringLiteral("ALPHA")) {
            return ma.value(QStringLiteral("name")).toString() < mb.value(QStringLiteral("name")).toString();
        }
        if (sortMode == QStringLiteral("COMPANY")) {
            const QString nameA = ma.value(QStringLiteral("name")).toString();
            const QString nameB = mb.value(QStringLiteral("name")).toString();
            const int atA = nameA.indexOf(QStringLiteral(" - "));
            const int atB = nameB.indexOf(QStringLiteral(" - "));
            const QString compA = (atA >= 0) ? nameA.left(atA) : nameA;
            const QString compB = (atB >= 0) ? nameB.left(atB) : nameB;
            if (compA != compB) return compA < compB;
            return nameA < nameB;
        }
        // SYSTEM (default)
        const QString snA = ma.value(QStringLiteral("shortName")).toString();
        const QString snB = mb.value(QStringLiteral("shortName")).toString();
        const QString sa = snA.isEmpty() ? ma.value(QStringLiteral("name")).toString() : snA;
        const QString sb = snB.isEmpty() ? mb.value(QStringLiteral("name")).toString() : snB;
        return sa < sb;
    });

    return playlists;
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
    if (!f.open(QIODevice::ReadOnly)) return entries;

    QByteArray data = f.readAll();
    if (data.isEmpty()) return entries;

    qInfo("[RetroArch] Parsing playlist: %s (size: %d bytes)", qPrintable(path), data.size());

    // Check for RZIP-compressed playlist (RetroArch 1.7.5+ default).
    // Header is 20 bytes:
    //   bytes 0-5:  magic "#RZIP"
    //   byte 6:     version (1=deflate, 2=zstd)
    //   byte 7:     '#' (0x23)
    //   bytes 8-11: chunk size (big-endian)
    //   bytes 12-19: total uncompressed size (big-endian)
    // Then chunks: 4-byte LE compressed_size + zlib-compressed data.
    if (data.size() >= 20 &&
        data[0] == '#' && data[1] == 'R' && data[2] == 'Z' && data[3] == 'I' &&
        data[4] == 'P' && data[5] == 'v' && data[6] == 1 && data[7] == '#') {

        // Read total uncompressed size (little-endian, bytes 12-19).
        // Cast to quint8 first to avoid sign-extension from signed char.
        quint64 totalSize = (quint64(quint8(data[12]))
                           | (quint64(quint8(data[13])) << 8)
                           | (quint64(quint8(data[14])) << 16)
                           | (quint64(quint8(data[15])) << 24)
                           | (quint64(quint8(data[16])) << 32)
                           | (quint64(quint8(data[17])) << 40)
                           | (quint64(quint8(data[18])) << 48)
                           | (quint64(quint8(data[19])) << 56));

        qInfo("[RetroArch] RZIP header: total_size=%llu, data.size()=%d",
              (qlonglong)totalSize, data.size());

        // Parse chunks: each chunk = 4-byte LE compressed_size + zlib data
        QByteArray decompressed;
        int offset = 20;
        int chunkCount = 0;
        quint32 nominalChunkSize = (quint32(quint8(data[11])) << 24)
                                 | (quint32(quint8(data[10])) << 16)
                                 | (quint32(quint8(data[9])) << 8)
                                 | quint32(quint8(data[8]));

        while (offset + 4 <= data.size()) {
            // Read 4-byte chunk header (LE). Cast to quint8 to avoid sign-extension.
            quint32 chunkCompSize = (quint32(quint8(data[offset + 3])) << 24)
                                  | (quint32(quint8(data[offset + 2])) << 16)
                                  | (quint32(quint8(data[offset + 1])) << 8)
                                  | quint32(quint8(data[offset]));
            offset += 4;

            if (chunkCompSize == 0) {
                qWarning("[RetroArch] Zero chunk size at offset %d", offset);
                break;
            }

            // Read compressed chunk data (offset already advanced past 4-byte LE size header)
            QByteArray chunkData = data.mid(offset, chunkCompSize);
            offset += chunkCompSize;

            // Decompress with Qt's qUncompress. Real RetroArch chunks have raw
            // zlib data (starts with 0x78 zlib header byte); test chunks built
            // via qCompress already prepend Qt's 4-byte uncompressed size header.
            // Check the first byte: if 0x78, prepend the Qt header; otherwise pass
            // directly to qUncompress.
            QByteArray chunkDecompressed;
            if (chunkData.size() >= 2 && (quint8)chunkData[0] == 0x78) {
                QByteArray chunkWithHeader;
                chunkWithHeader.resize(4);
                chunkWithHeader[0] = (quint8)(nominalChunkSize & 0xFF);
                chunkWithHeader[1] = (quint8)((nominalChunkSize >> 8) & 0xFF);
                chunkWithHeader[2] = (quint8)((nominalChunkSize >> 16) & 0xFF);
                chunkWithHeader[3] = (quint8)((nominalChunkSize >> 24) & 0xFF);
                chunkWithHeader.append(chunkData);
                chunkDecompressed = qUncompress(chunkWithHeader);
            } else {
                chunkDecompressed = qUncompress(chunkData);
            }
            if (!chunkDecompressed.isEmpty()) {
                decompressed.append(chunkDecompressed);
                chunkCount++;
            } else {
                qWarning("[RetroArch] Failed to decompress chunk %d at offset %d",
                         chunkCount + 1, offset - chunkCompSize);
                break;
            }
        }

        qInfo("[RetroArch] Decompressed %d chunks, total %d bytes (expected: %llu)",
              chunkCount, decompressed.size(), (qlonglong)totalSize);

        if (!decompressed.isEmpty()) {
            data = decompressed;
            qInfo("[RetroArch] Successfully decompressed RZIP playlist");
        } else {
            qWarning("[RetroArch] Failed to decompress RZIP playlist: %s", qPrintable(path));
            return entries;
        }
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("[RetroArch] JSON parse error: %s", qPrintable(err.errorString()));
        return entries;
    }

    QJsonArray items = doc.object()["items"].toArray();
    // Extract the playlist's default core path — used to resolve "DETECT" entries.
    const QString defaultCorePath = doc.object()["default_core_path"].toString();
    qInfo("[RetroArch] Default core path: %s", qPrintable(defaultCorePath));
    qInfo("[RetroArch] Parsed %d items from playlist", items.size());
    for (const QJsonValue &val : items) {
        QJsonObject item = val.toObject();
        const QString label = item["label"].toString();
        const QString romPath = item["path"].toString();
        if (label.isEmpty() || romPath.isEmpty()) continue;

        entries.append(QVariantMap{
            // Thumbnail resolution moved out of parsePlaylist: it scanned the
            // thumbnail tree for every game in every playlist at module open
            // (~9s for 4.7k games). QML resolves per visible row via
            // retroarchBackend.resolveThumbnail(name, playlistBasename).
            { "name", label },
            { "romPath", resolveRomPath(path, romPath) },
            { "corePath", resolveCorePath(item["core_path"].toString(), defaultCorePath) },
            { "coreName", QFileInfo(item["core_path"].toString()).baseName() },
            { "dbName", item["db_name"].toString() },
            // Thumbnail DBs are keyed by the playlist file's BASENAME WITH
            // ORIGINAL CASE (RetroArch's own convention), while systemId is
            // lower-cased for our internal identity. Keep them separate so
            // any install — named or human-playlists — resolves.
            { "thumbnail", QString() },
            { "systemId", systemId }
        });
    }
    return entries;
}

QString RetroArchBackend::resolveRomPath(const QString &playlistPath,
                                         const QString &romPath) const {
    // Absolute paths are used as-is — the playlist is the source of truth.
    if (QFileInfo(romPath).isAbsolute()) return romPath;

    // RetroArch stores archive-internal entries as "archive.zip#entry.rom".
    // The archive part is what must exist on disk; the whole string is what
    // RetroArch needs to open it (it handles the '#' itself). Check the
    // archive, not the composite path — QFileInfo treats "zip#entry" as a
    // literal filename and always reports it missing.
    const int hashAt = romPath.indexOf(QLatin1Char('#'));
    const QString archivePart = (hashAt >= 0) ? romPath.left(hashAt) : romPath;

    auto archiveResolved = [&](const QString &candidate) -> QString {
        // Returns the path to pass to RetroArch when the archive exists at
        // (or relative to) one of the known bases; empty string otherwise.
        const QString full = QFileInfo(candidate).isAbsolute()
                            ? candidate : QFileInfo(playlistPath).absoluteDir().filePath(candidate);
        if (QFileInfo(full).exists())
            return (hashAt >= 0) ? romPath : full;
        // QDir::filePath passes absolute paths through, so try the relative
        // candidate against the ROM root separately.
        const QString underRom = QFileInfo(candidate).isAbsolute()
                                ? QString() : QDir(m_romDir).filePath(candidate);
        if (!underRom.isEmpty() && QFileInfo(underRom).exists())
            return (hashAt >= 0) ? QDir(m_romDir).filePath(romPath) : underRom;
        return QString();
    };

    const QString resolvedArchive = archiveResolved(archivePart);
    if (!resolvedArchive.isEmpty())
        return resolvedArchive;

    // Relative paths are stored relative to RetroArch's config dir (where the
    // playlists live), so try the playlist directory first…
    QFileInfo plInfo(playlistPath);
    QString candidate = plInfo.absoluteDir().filePath(romPath);
    if (QFileInfo(candidate).exists()) return candidate;

    // …then the configured ROM root (people commonly keep relative paths
    // rooted at their ROM folder).
    candidate =QDir(m_romDir).filePath(romPath);
    if (QFileInfo(candidate).exists()) return candidate;

    // Nothing on disk — return as-is so RetroArch still gets the path the
    // playlist recorded, and its own logging tells the user it's missing.
    return romPath;
}

QString RetroArchBackend::resolveCorePath(const QString &corePath, const QString &defaultCorePath) const {
    // "DETECT" means RetroArch should figure out the core at runtime. We can't
    // leave it as-is — RetroArch won't find the core without an explicit path.
    // Fall back to the playlist's default_core_path, which RetroArch wrote when
    // the playlist was created.
    if (corePath == "DETECT" && !defaultCorePath.isEmpty()) {
        return defaultCorePath;
    }
    // Absolute: pass through. Relative filename: pass through too — RetroArch
    // resolves it against its own core directory (core_directory setting),
    // which is exactly where a user-installed core lives. Guessing a path
    // ourselves would break on any install layout we haven't anticipated.
    return corePath;
}

QString RetroArchBackend::findThumbnail(const QString &gameName,
                                        const QString &dirName) const {
    // RetroArch organises thumbnails as
    // <thumbnails>/<playlist basename, original case>/<Type>/<name>.png,
    // Type being one of Named_Boxarts, Named_Snaps, Named_Titles, Boxarts, …
    // — the "Named_" variants match the playlist label. dirName is the
    // playlist basename WITH CASE PRESERVED; never pass the lower-cased
    // internal systemId here.
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
        const QDir thumbDir(m_thumbnailDir + QStringLiteral("/") + dirName + QStringLiteral("/") + type);
        if (!thumbDir.exists()) continue;

        // Exact, then RetroArcName-sanitized (thumbnail DBs mangle "/\\:*?|<>[]"
        // etc. to "_" when generated from labels), then case-insensitive —
        // three escalating attempts without pre-listing the directory.
        const QString sanitized = sanitizeForThumbnailDb(gameName);
        for (const QString &candidate : { gameName, sanitized }) {
            for (const QString &ext : extensions) {
                const QString exact = thumbDir.filePath(candidate + ext);
                if (QFileInfo(exact).exists()) return exact;
            }
        }

        const QFileInfoList files = thumbDir.entryInfoList(globs, QDir::Files);
        for (const QFileInfo &file : files) {
            const QString base = file.completeBaseName();
            if (base.compare(gameName, Qt::CaseInsensitive) == 0
                || base.compare(sanitizeForThumbnailDb(gameName), Qt::CaseInsensitive) == 0)
                return file.absoluteFilePath();
        }
    }
    return QString();
}

QString RetroArchBackend::resolveThumbnail(const QString &gameName,
                                          const QString &playlistBasename) const {
    return findThumbnail(gameName, playlistBasename);
}

// Same character mangling RetroArch applies when naming thumbnail DB files
// from labels: reserved path characters and brackets collapse to "_".
QString RetroArchBackend::sanitizeForThumbnailDb(const QString &label) const {
    QString out;
    for (const QChar c : label) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')
            || c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('|')
            || c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('[')
            || c == QLatin1Char(']') || c == QLatin1Char('\n') || c == QLatin1Char('\t'))
            out += QLatin1Char('_');
        else
            out += c;
    }
    return out;
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
    // Archive entries ("archive.zip#entry.rom") are recorded as one string;
    // QFileInfo would treat it as a literal filename and report it missing.
    // The archive itself is what has to exist — RetroArch opens the entry.
    const int hashAt = romPath.indexOf(QLatin1Char('#'));
    const QString onDisk = (hashAt >= 0) ? romPath.left(hashAt) : romPath;
    if (!QFileInfo(onDisk).exists()) {
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
