#include "../RetroArchBackend.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

// Unit tests for RetroArchBackend's playlist browsing. Everything is local
// file I/O against a temp tree, so the suite is fast and needs no RetroArch
// install, no display and no network.
class RetroArchBackendTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    // A temp-tree config.json pointing every path at the fixture dirs.
    RetroArchBackend *makeBackend(const QString &playlistSort = QString());
    void writePlaylist(const QString &name, const QString &content);
    void cleanPlaylistDir();
    void writePlaylistBinary(const QString &name, const QByteArray &content);
    static QString playlistJson(const QString &corePath, const QStringList &labels);

private slots:
    void initTestCase();
    void systemFromPlaylistIsLowercaseBaseName();
    void refreshSystemsGroupsAndSorts();
    void refreshSystemsOnMissingDirIsEmpty();
    void refreshPlaylistsFiltersBySystem();
    void refreshGamesParsesEntries();
    void refreshGamesSkipsMalformedItems();
    void refreshGamesOnBrokenJsonIsEmpty();
    void refreshGamesParsesRzipCompressedPlaylist();
    void relativeRomResolvesAgainstRomDir();
    void absoluteRomPassesThrough();
    void thumbnailFoundInNamedBoxarts();
    void missingThumbnailYieldsEmptyString();
    void thumbnailDbUsesPlaylistBasenameOriginalCase();
    void sanitizedLabelMatchesSanitizedDbName();
    void signalsFireAlongsideReturnValues();
    void archiveRomPathChecksArchiveOnly();
    void archiveRomPathRelativeResolvesAgainstRomDir();
    void plainRomPathStillResolves();
    void playlistSortSystemIsDefault();
    void playlistSortAlpha();
    void playlistSortCompanyFallsBackToAlpha();
};

void RetroArchBackendTest::initTestCase() {
    QVERIFY(m_tmp.isValid());
    QDir().mkpath(m_tmp.path() + "/playlists");
    QDir().mkpath(m_tmp.path() + "/ROMs");
    QDir().mkpath(m_tmp.path() + "/thumbnails");
}

RetroArchBackend *RetroArchBackendTest::makeBackend(const QString &playlistSort) {
    const QString dataRoot = m_tmp.path();
    QJsonObject mod;
    mod.insert("playlist_directory", dataRoot + "/playlists");
    mod.insert("rom_directory", dataRoot + "/ROMs");
    mod.insert("thumbnail_directory", dataRoot + "/thumbnails");
    if (!playlistSort.isEmpty())
        mod.insert("playlist_sort", playlistSort);
    QJsonObject cfg;
    cfg.insert("modules", QJsonObject{ { QStringLiteral("com.240mp.retroarch"), mod } });
    {
        QFile f(dataRoot + "/config.json");
        // QVERIFY would return out of this non-slot helper, so fail closed
        // instead: slots check the pointer.
        if (!f.open(QIODevice::WriteOnly))
            return nullptr;
        f.write(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    }
    return new RetroArchBackend(dataRoot, dataRoot, nullptr, this);
}

void RetroArchBackendTest::writePlaylist(const QString &name, const QString &content) {
    const QString path = m_tmp.path() + "/playlists/" + name;
    QDir().mkpath(m_tmp.path() + "/playlists");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content.toUtf8());
}

void RetroArchBackendTest::writePlaylistBinary(const QString &name, const QByteArray &content) {
    const QString path = m_tmp.path() + "/playlists/" + name;
    QDir().mkpath(m_tmp.path() + "/playlists");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QString RetroArchBackendTest::playlistJson(const QString &corePath,
                                           const QStringList &labels) {
    QJsonArray items;
    for (const QString &label : labels) {
        QJsonObject item;
        item.insert("label", label);
        item.insert("path", label + ".nes");
        if (!corePath.isEmpty())
            item.insert("core_path", corePath);
        items.append(item);
    }
    QJsonObject root;
    root.insert("version", 1);
    root.insert("items", items);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void RetroArchBackendTest::systemFromPlaylistIsLowercaseBaseName() {
    RetroArchBackend be(m_tmp.path(), m_tmp.path(), nullptr, this);
    QCOMPARE(be.systemFromPlaylist("/x/playlists/Snes9x_libretro.json"),
             QString("snes9x_libretro"));
    QCOMPARE(be.systemFromPlaylist("/x/playlists/NES.lpl"), QString("nes"));
}

void RetroArchBackendTest::refreshSystemsGroupsAndSorts() {
    RetroArchBackend *be = makeBackend();
    writePlaylist("snes9x_libretro.json",
                  playlistJson("/cores/snes9x_libretro.so",
                               { "Super Mario World", "Chrono Trigger" }));
    writePlaylist("nes_libretro.json",
                  playlistJson("/cores/nes_libretro.so", { "Tetris" }));
    writePlaylist("genesis_libretro.json",
                  playlistJson("/cores/genesis_libretro.so", { "Sonic 2" }));

    const QVariantList systems = be->refreshSystems();
    QCOMPARE(systems.size(), 3);
    QCOMPARE(systems.at(0).toMap()["name"].toString(), QString("genesis_libretro"));
    QCOMPARE(systems.at(1).toMap()["name"].toString(), QString("nes_libretro"));
    QCOMPARE(systems.at(2).toMap()["name"].toString(), QString("snes9x_libretro"));
    // Game counts propagate.
    QCOMPARE(systems.at(2).toMap()["count"].toInt(), 2);
    QCOMPARE(systems.at(1).toMap()["count"].toInt(), 1);
}

void RetroArchBackendTest::refreshSystemsOnMissingDirIsEmpty() {
    // Isolated config: points at a playlist dir that never exists, so this
    // test can't clobber the shared fixture the other tests rely on.
    const QString dataRoot = m_tmp.path();
    QJsonObject mod;
    mod.insert("playlist_directory", dataRoot + "/no-such-dir");
    QJsonObject cfg;
    cfg.insert("modules", QJsonObject{ { QStringLiteral("com.240mp.retroarch"), mod } });
    {
        QFile f(dataRoot + "/config.json");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    }
    RetroArchBackend be(dataRoot, dataRoot, nullptr, this);
    QVERIFY(be.refreshSystems().isEmpty());
    // Restore the shared config for the next test.
    {
        QJsonObject mod2;
        mod2.insert("playlist_directory", dataRoot + "/playlists");
        mod2.insert("rom_directory", dataRoot + "/ROMs");
        mod2.insert("thumbnail_directory", dataRoot + "/thumbnails");
        QJsonObject cfg2;
        cfg2.insert("modules", QJsonObject{ { QStringLiteral("com.240mp.retroarch"), mod2 } });
        QFile f(dataRoot + "/config.json");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(cfg2).toJson(QJsonDocument::Compact));
    }
}

void RetroArchBackendTest::refreshPlaylistsFiltersBySystem() {
    RetroArchBackend *be = makeBackend();
    writePlaylist("snes9x_libretro.json",
                  playlistJson("/cores/snes9x_libretro.so", { "A", "B" }));
    writePlaylist("nes_libretro.json",
                  playlistJson("/cores/nes_libretro.so", { "C" }));

    const QVariantList snes = be->refreshPlaylists("snes9x_libretro");
    QCOMPARE(snes.size(), 1);
    QCOMPARE(snes.at(0).toMap()["name"].toString(), QString("snes9x_libretro"));
    QCOMPARE(snes.at(0).toMap()["count"].toInt(), 2);

    const QVariantList nothing = be->refreshPlaylists("psx_libretro");
    QVERIFY(nothing.isEmpty());
}

void RetroArchBackendTest::refreshGamesParsesEntries() {
    RetroArchBackend *be = makeBackend();
    const QString core = "/cores/snes9x_libretro.so";
    writePlaylist("snes9x_libretro.json",
                  playlistJson(core, { "Super Mario World", "Chrono Trigger" }));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/snes9x_libretro.json");
    QCOMPARE(games.size(), 2);
    QCOMPARE(games.at(0).toMap()["name"].toString(), QString("Super Mario World"));
    QCOMPARE(games.at(0).toMap()["corePath"].toString(), core);
    QCOMPARE(games.at(0).toMap()["coreName"].toString(), QString("snes9x_libretro"));
    QCOMPARE(games.at(1).toMap()["name"].toString(), QString("Chrono Trigger"));
    // parsePlaylist no longer resolves thumbnails (lazy — see resolveThumbnail).
    QVERIFY(games.at(0).toMap()["thumbnail"].toString().isEmpty());
}

void RetroArchBackendTest::refreshGamesSkipsMalformedItems() {
    RetroArchBackend *be = makeBackend();
    QJsonArray items;
    QJsonObject good;
    good.insert("label", "Ok Game");
    good.insert("path", "Ok Game.nes");
    QJsonObject noPath;
    noPath.insert("label", "Missing Path");
    QJsonObject noLabel;
    noLabel.insert("path", "Missing Label.nes");
    items << good << noPath << noLabel;
    QJsonObject root;
    root.insert("items", items);

    writePlaylist("nes_libretro.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));
    const QVariantList games = be->refreshGames(m_tmp.path() + "/playlists/nes_libretro.json");
    QCOMPARE(games.size(), 1);
    QCOMPARE(games.at(0).toMap()["name"].toString(), QString("Ok Game"));
}

void RetroArchBackendTest::refreshGamesOnBrokenJsonIsEmpty() {
    RetroArchBackend *be = makeBackend();
    writePlaylist("broken.json", "definitely not json {");
    QVERIFY(be->refreshGames(m_tmp.path() + "/playlists/broken.json").isEmpty());
}

void RetroArchBackendTest::relativeRomResolvesAgainstRomDir() {
    RetroArchBackend *be = makeBackend();
    const QString roms = m_tmp.path() + "/ROMs";
    QDir().mkpath(roms + "/snes");
    {
        QFile rom(roms + "/snes/Chrono Trigger.smc");
        QVERIFY(rom.open(QIODevice::WriteOnly));
    }
    QJsonArray items;
    QJsonObject item;
    item.insert("label", "Chrono Trigger");
    item.insert("path", "snes/Chrono Trigger.smc");
    items.append(item);
    QJsonObject root;
    root.insert("items", items);
    writePlaylist("snes9x_libretro.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/snes9x_libretro.json");
    QCOMPARE(games.size(), 1);
    QCOMPARE(games.at(0).toMap()["romPath"].toString(),
             roms + "/snes/Chrono Trigger.smc");
}

void RetroArchBackendTest::absoluteRomPassesThrough() {
    RetroArchBackend *be = makeBackend();
    QJsonArray items;
    QJsonObject item;
    item.insert("label", "Absolute");
    item.insert("path", "/some/where/Absolute.nes");
    items.append(item);
    QJsonObject root;
    root.insert("items", items);
    writePlaylist("nes_libretro.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/nes_libretro.json");
    QCOMPARE(games.at(0).toMap()["romPath"].toString(), QString("/some/where/Absolute.nes"));
}

void RetroArchBackendTest::thumbnailFoundInNamedBoxarts() {
    RetroArchBackend *be = makeBackend();
    const QString thumbs = m_tmp.path() + "/thumbnails";
    // RetroArch's real layout: <thumbnails>/<system>/<type>/<name>.png
    // The system comes from the playlist filename (lowercased).
    QDir().mkpath(thumbs + "/snes9x_libretro/Named_Boxarts");
    {
        QFile f(thumbs + "/snes9x_libretro/Named_Boxarts/Super Mario World.png");
        // Same case as the label: lookup via exists() must resolve on both
        // case-sensitive (Linux) and case-insensitive (macOS) filesystems, and
        // the returned path must be exactly the file on disk.
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    writePlaylist("snes9x_libretro.json",
                  playlistJson("/cores/snes9x_libretro.so", { "Super Mario World" }));

    // Lazy API: QML calls this per rendered row instead of parsePlaylist
    // scanning the tree for every game.
    QCOMPARE(be->resolveThumbnail("Super Mario World", "snes9x_libretro"),
             thumbs + "/snes9x_libretro/Named_Boxarts/Super Mario World.png");
    QVERIFY(be->resolveThumbnail("Not In The DB", "snes9x_libretro").isEmpty());
}

void RetroArchBackendTest::missingThumbnailYieldsEmptyString() {
    RetroArchBackend *be = makeBackend();
    writePlaylist("nes_libretro.json",
                  playlistJson("/cores/nes_libretro.so", { "No Art Game" }));
    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/nes_libretro.json");
    QVERIFY(games.at(0).toMap()["thumbnail"].toString().isEmpty());
}

void RetroArchBackendTest::thumbnailDbUsesPlaylistBasenameOriginalCase() {
    // The generalization: on case-sensitive filesystems a lower-cased key
    // against a mixed-case DB directory is a hard miss. Real RetroArch
    // installs name both playlists and thumbnail DBs with human casing.
    RetroArchBackend *be = makeBackend();
    const QString thumbs = m_tmp.path() + "/thumbnails";
    QDir().mkpath(thumbs + "/Nintendo - Test Console/Named_Boxarts");
    {
        QFile f(thumbs + "/Nintendo - Test Console/Named_Boxarts/Cool Game.png");
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    writePlaylist("Nintendo - Test Console.lpl",
                  playlistJson("/cores/test_libretro.so", { "Cool Game" }));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/Nintendo - Test Console.lpl");
    QVERIFY(!games.isEmpty());
    // Original-case playlist basename is what the thumbnail DB dir is named.
    QCOMPARE(be->resolveThumbnail("Cool Game", "Nintendo - Test Console"),
             thumbs + "/Nintendo - Test Console/Named_Boxarts/Cool Game.png");
}

void RetroArchBackendTest::sanitizedLabelMatchesSanitizedDbName() {
    // Thumbnail DB file names mangle label characters ("/" "[]" etc.) to "_".
    // The lookup must apply the same rule, so any install's DB resolves —
    // no fixture-specific name is hardcoded.
    RetroArchBackend *be = makeBackend();
    const QString thumbs = m_tmp.path() + "/thumbnails";
    QDir().mkpath(thumbs + "/nes_libretro/Named_Boxarts");
    {
        QFile f(thumbs + "/nes_libretro/Named_Boxarts/F_Z Quest _T_S_.png");
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    writePlaylist("nes_libretro.json",
                  playlistJson("/cores/nes_libretro.so", { "F/Z Quest [T/S]" }));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/nes_libretro.json");
    QVERIFY(!games.isEmpty());
    QCOMPARE(be->resolveThumbnail("F/Z Quest [T/S]", "nes_libretro"),
             thumbs + "/nes_libretro/Named_Boxarts/F_Z Quest _T_S_.png");
}

void RetroArchBackendTest::signalsFireAlongsideReturnValues() {
    // Clean the playlist dir: previous tests left files there, which would
    // make this count-based assertion flaky.
    QDir plDir(m_tmp.path() + "/playlists");
    plDir.removeRecursively();
    QDir().mkpath(plDir.path());

    RetroArchBackend *be = makeBackend();
    writePlaylist("snes9x_libretro.json",
                  playlistJson("/cores/snes9x_libretro.so", { "A" }));

    QSignalSpy systemsSpy(be, &RetroArchBackend::systemsReady);
    QSignalSpy gamesSpy(be, &RetroArchBackend::gamesReady);
    be->refreshSystems();
    be->refreshGames(m_tmp.path() + "/playlists/snes9x_libretro.json");

    QCOMPARE(systemsSpy.count(), 1);
    QCOMPARE(gamesSpy.count(), 1);
    QCOMPARE(systemsSpy.at(0).at(0).toList().size(), 1);
    QCOMPARE(gamesSpy.at(0).at(0).toList().size(), 1);
}

void RetroArchBackendTest::refreshGamesParsesRzipCompressedPlaylist() {
    RetroArchBackend *be = makeBackend();

    // Build a JSON playlist, then wrap it in RZIP format.
    // RZIP header: "#RZIP#" (8) + chunk_size(4, LE) + total_size(8, LE) = 20 bytes
    // Then chunks: compressed_size(4, LE) + zlib-compressed data.
    QJsonArray items;
    {
        QJsonObject item1;
        item1.insert("label", "Super Mario World");
        item1.insert("path", "snes/Super Mario World.smc");
        item1.insert("core_path", "snes9x_libretro.so");
        items.append(item1);
        QJsonObject item2;
        item2.insert("label", "Chrono Trigger");
        item2.insert("path", "snes/Chrono Trigger.smc");
        item2.insert("core_path", "snes9x_libretro.so");
        items.append(item2);
    }
    QJsonObject root;
    root.insert("version", 1);
    root.insert("items", items);
    QString jsonStr = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QByteArray jsonBytes = jsonStr.toUtf8();

    // Compress with zlib (Qt's qUncompress expects this format)
    QByteArray compressed = qCompress(jsonBytes, 6);

    // Build RZIP header (20 bytes)
    QByteArray header;
    header.append('#').append('R').append('Z').append('I').append('P')  // bytes 0-4: "#RZIP"
          .append('v')  // byte 5: 'v'
          .append((quint8)1)  // byte 6: version = 1 (deflate)
          .append('#');  // byte 7: '#'
    // chunk_size (4 bytes, LE) — unused by reader, just set to 128KB
    header.append((quint8)0).append((quint8)0).append((quint8)2).append((quint8)0);
    // total_size (8 bytes, LE)
    quint64 totalSize = jsonBytes.size();
    for (int i = 0; i < 8; ++i)
        header.append((quint8)(totalSize >> (i * 8)));

    // Chunk header: compressed_size (4 bytes, LE)
    QByteArray chunkHeader;
    chunkHeader.append((quint8)(compressed.size() & 0xFF))
               .append((quint8)((compressed.size() >> 8) & 0xFF))
               .append((quint8)((compressed.size() >> 16) & 0xFF))
               .append((quint8)((compressed.size() >> 24) & 0xFF));

    // qCompress already adds a 4-byte header (original size LE) before zlib data.
    // qUncompress expects this same format, so pass directly.
    QByteArray rzipData = header + chunkHeader + compressed;

    writePlaylistBinary("snes9x_libretro.rpl", rzipData);

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/snes9x_libretro.rpl");
    QCOMPARE(games.size(), 2);
    QCOMPARE(games.at(0).toMap()["name"].toString(), QString("Super Mario World"));
    QCOMPARE(games.at(1).toMap()["name"].toString(), QString("Chrono Trigger"));
}

void RetroArchBackendTest::archiveRomPathChecksArchiveOnly() {
    // RetroArch records zipped multi-file sets as "archive.zip#entry.rom".
    // Only the archive is checked on disk; the full string is passed through
    // so RetroArch can open the entry itself.
    RetroArchBackend *be = makeBackend();
    const QString roms = m_tmp.path() + "/ROMs";
    QDir().mkpath(roms + "/snes");
    {
        QFile zip(roms + "/snes/Aaahh Real Monsters (USA).zip");
        QVERIFY(zip.open(QIODevice::WriteOnly));
    }
    QJsonArray items;
    QJsonObject item;
    item.insert("label", "Aaahh!!! Real Monsters");
    item.insert("path", "snes/Aaahh Real Monsters (USA).zip#Aaahh Real Monsters (USA).sfc");
    items.append(item);
    QJsonObject root;
    root.insert("items", items);
    writePlaylist("snes9x_libretro.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/snes9x_libretro.json");
    QCOMPARE(games.size(), 1);
    // Archive found under the ROM dir: entry suffix must be preserved.
    QCOMPARE(games.at(0).toMap()["romPath"].toString(),
             roms + "/snes/Aaahh Real Monsters (USA).zip#Aaahh Real Monsters (USA).sfc");
}

void RetroArchBackendTest::archiveRomPathRelativeResolvesAgainstRomDir() {
    RetroArchBackend *be = makeBackend();
    const QString roms = m_tmp.path() + "/ROMs";
    QDir().mkpath(roms);
    {
        QFile zip(roms + "/Game (USA).zip");
        QVERIFY(zip.open(QIODevice::WriteOnly));
    }
    QJsonArray items;
    QJsonObject item;
    item.insert("label", "Game");
    item.insert("path", "Game (USA).zip#Game (USA).nes");
    items.append(item);
    QJsonObject root;
    root.insert("items", items);
    writePlaylist("nes_libretro.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/nes_libretro.json");
    QCOMPARE(games.size(), 1);
    QCOMPARE(games.at(0).toMap()["romPath"].toString(),
             roms + "/Game (USA).zip#Game (USA).nes");
}

void RetroArchBackendTest::plainRomPathStillResolves() {
    // Non-archive entries (loose files, whole-zip FBNeo sets) keep the old
    // behavior: resolve against playlist dir then ROM dir.
    RetroArchBackend *be = makeBackend();
    const QString roms = m_tmp.path() + "/ROMs";
    QDir().mkpath(roms);
    {
        QFile rom(roms + "/gtmr.7z");
        QVERIFY(rom.open(QIODevice::WriteOnly));
    }
    QJsonArray items;
    QJsonObject item;
    item.insert("label", "Gate of Thunder");
    item.insert("path", "gtmr.7z");
    items.append(item);
    QJsonObject root;
    root.insert("items", items);
    writePlaylist("fbneo - arcade games.json",
                  QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QVariantList games =
        be->refreshGames(m_tmp.path() + "/playlists/fbneo - arcade games.json");
    QCOMPARE(games.size(), 1);
    QCOMPARE(games.at(0).toMap()["romPath"].toString(), roms + "/gtmr.7z");
}

void RetroArchBackendTest::cleanPlaylistDir() {
    QDir plDir(m_tmp.path() + "/playlists");
    plDir.removeRecursively();
    QDir().mkpath(plDir.path());
}

void RetroArchBackendTest::playlistSortSystemIsDefault() {
    cleanPlaylistDir();
    RetroArchBackend *be = makeBackend(); // no playlist_sort -> SYSTEM
    writePlaylist("Sega - Mega Drive - Genesis.lpl",
                  playlistJson("/cores/genesis_plus_gx_libretro.so", { "Sonic 2" }));
    writePlaylist("Nintendo - Super Nintendo Entertainment System.lpl",
                  playlistJson("/cores/bsnes_libretro.so", { "Chrono Trigger" }));
    writePlaylist("Nintendo - Game Boy.lpl",
                  playlistJson("/cores/mgba_libretro.so", { "Tetris" }));

    const QVariantList pls = be->refreshAllPlaylists();
    QCOMPARE(pls.size(), 3);
    // SYSTEM (default), case-sensitive: "GB" is a prefix of "GENESIS", so it
    // sorts first; SNES last.
    QCOMPARE(pls.at(0).toMap()["shortName"].toString(), QString("GB"));
    QCOMPARE(pls.at(1).toMap()["shortName"].toString(), QString("GENESIS"));
    QCOMPARE(pls.at(2).toMap()["shortName"].toString(), QString("SNES"));
}

void RetroArchBackendTest::playlistSortAlpha() {
    cleanPlaylistDir();
    RetroArchBackend *be = makeBackend(QStringLiteral("ALPHA"));
    writePlaylist("Sega - Mega Drive - Genesis.lpl",
                  playlistJson("/cores/genesis_plus_gx_libretro.so", { "Sonic 2" }));
    writePlaylist("Nintendo - Super Nintendo Entertainment System.lpl",
                  playlistJson("/cores/bsnes_libretro.so", { "Chrono Trigger" }));
    writePlaylist("Nintendo - Game Boy.lpl",
                  playlistJson("/cores/mgba_libretro.so", { "Tetris" }));

    const QVariantList pls = be->refreshAllPlaylists();
    QCOMPARE(pls.size(), 3);
    // ALPHA: raw basenames, N < S
    QCOMPARE(pls.at(0).toMap()["name"].toString(), QString("Nintendo - Game Boy"));
    QCOMPARE(pls.at(1).toMap()["name"].toString(),
             QString("Nintendo - Super Nintendo Entertainment System"));
    QCOMPARE(pls.at(2).toMap()["name"].toString(), QString("Sega - Mega Drive - Genesis"));
}

void RetroArchBackendTest::playlistSortCompanyFallsBackToAlpha() {
    cleanPlaylistDir();
    RetroArchBackend *be = makeBackend(QStringLiteral("COMPANY"));
    writePlaylist("Sega - Mega Drive - Genesis.lpl",
                  playlistJson("/cores/genesis_plus_gx_libretro.so", { "Sonic 2" }));
    writePlaylist("Sega - Game Gear.lpl",
                  playlistJson("/cores/genesis_plus_gx_libretro.so", { "Sonic 1" }));
    writePlaylist("Nintendo - Game Boy.lpl",
                  playlistJson("/cores/mgba_libretro.so", { "Tetris" }));
    // No " - " separator: the whole name is the company key (fallback rule).
    writePlaylist("nes.lpl",
                  playlistJson("/cores/nes_libretro.so", { "Gate of Thunder" }));

    const QVariantList pls = be->refreshAllPlaylists();
    QCOMPARE(pls.size(), 4);
    // COMPANY: Nintendo < Sega (x2, alpha within company) < nes (no prefix)
    QCOMPARE(pls.at(0).toMap()["name"].toString(), QString("Nintendo - Game Boy"));
    QCOMPARE(pls.at(1).toMap()["name"].toString(), QString("Sega - Game Gear"));
    QCOMPARE(pls.at(2).toMap()["name"].toString(), QString("Sega - Mega Drive - Genesis"));
    QCOMPARE(pls.at(3).toMap()["name"].toString(), QString("nes"));
}

QTEST_GUILESS_MAIN(RetroArchBackendTest)
#include "RetroArchBackendTest.moc"
