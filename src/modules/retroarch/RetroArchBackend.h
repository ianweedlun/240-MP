#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QProcess>
#include <QHash>
#include <QStringList>
#include <QThread>
#include <QMutex>

#include "RecentGamesStore.h"
#include "ThumbnailManifestCache.h"

class DisplayHandoff;
class QFileInfo;
class QThread;

// Browsing RetroArch's playlist library.
//
// This RetroArch install uses human-named playlist files (one per system),
// e.g. "Sega - Mega Drive - Genesis.lpl". The system id is derived from the
// filename without extension, lowercased. The playlist is the file itself,
// and its items are the games. Everything is read from local files — the only
// network-free module, which makes it fast on the Pi and easy to test.
//
// Launch hands the display to RetroArch via DisplayHandoff (same VT/DRM dance
// as mpv) and passes the core path straight from the playlist item, so all
// per-system configuration stays where the user set it — inside RetroArch.
class RetroArchBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString retroarchPath READ retroarchPath NOTIFY retroarchPathChanged)
    // True while the launchGame() child is alive. NOTIFY so QML can block the
    // idle tracker for the whole game (IDLE-02) — polling alone never tells
    // Main.qml when the game starts or ends.
    Q_PROPERTY(bool gameRunning READ gameRunning NOTIFY gameRunningChanged)

public:
    explicit RetroArchBackend(const QString &appRoot, const QString &dataRoot,
                              DisplayHandoff *handoff, QObject *parent = nullptr);
    ~RetroArchBackend() override;

    QString retroarchPath() const { return m_retroarchPath; }

    // Current playlist sort mode (SYSTEM / ALPHA / COMPANY).
    Q_INVOKABLE QString playlistSort() const;
    // Current games-view sort direction (A-Z / Z-A).
    Q_INVOKABLE QString gameSort() const;

    // Pure ordering used by the games view: case-insensitive, leading English
    // articles ignored, ties broken by the original name. Exposed so the
    // ordering can be unit-tested without a playlist file or a view.
    static QVariantList sortedGames(const QVariantList &games, bool descending);

    // True while the RetroArch child process launched by launchGame() is alive.
    // InputManager consults this (the child owns the whole console — the app
    // reads the same evdev stream) so a ROM-quit Back doesn't double-handle as
    // an extra goBack() in the QML navigation stack.
    bool gameRunning() const { return m_gameProcess != nullptr; }

    // Path getters for the settings UI.
    Q_INVOKABLE QString playlist_directory() const;
    Q_INVOKABLE QString rom_directory() const;
    Q_INVOKABLE QString thumbnail_directory() const;
    Q_INVOKABLE QString retroarch_path() const;

    // Scan the playlist directory and group its playlists by system. The list
    // is returned directly (a fast local scan — no round trip needed) and also
    // emitted, so a view can take whichever it finds convenient.
    Q_INVOKABLE QVariantList refreshSystems();
    // All playlists across all systems (flat list for direct navigation).
    Q_INVOKABLE QVariantList refreshAllPlaylists();
    // The playlists belonging to one system.
    Q_INVOKABLE QVariantList refreshPlaylists(const QString &systemId);
    // Parse one playlist file into its game entries.
    Q_INVOKABLE QVariantList refreshGames(const QString &playlistPath);
    // Resolve a game's thumbnail on demand — called by QML for the visible
    // rows only, so opening a playlist never scans the thumbnail tree for
    // every game in it. Same rules as findThumbnail; covered by tests.
    Q_INVOKABLE QString resolveThumbnail(const QString &gameName,
                                         const QString &playlistBasename) const;

    // Manifest-cache observability for the per-navigation scan gate. The scan
    // count is the number of real directory listings done since the last
    // clear(); it must stop growing once the cache is warm.
    Q_INVOKABLE int thumbnailCacheScanCount() const { return m_thumbManifestCache.scanCount(); }
    Q_INVOKABLE int thumbnailCacheHitCount() const { return m_thumbManifestCache.hitCount(); }
    void clearThumbnailCache() { m_thumbManifestCache.clear(); }
    // Async-prewarm observability: the generation increments on every clear
    // (including a thumbnail_directory change); tests assert that a setting
    // change invalidates work captured under the previous generation.
    quint64 thumbnailCacheGeneration() const { return m_thumbManifestCache.generation(); }
    // Launch a game: hand the display to RetroArch, run it with the item's
    // core, restore the display when it exits. The map should carry the fields a
    // later "recently played" row needs (playlistPath, playlistBasename,
    // shortName); everything else is taken from the item itself.
    Q_INVOKABLE void launchGame(const QVariantMap &game, const QString &systemId);

    // Recently played, sorted by the Game Sort setting like any games list.
    // The RECENT row on the playlist screen and Recent.qml show this order;
    // lastPlayedGame() below still reports the actual newest launch.
    Q_INVOKABLE QVariantList recentGames() const;
    // One press from the main menu to the thing you were playing: {} when nothing
    // has been played yet.
    Q_INVOKABLE QVariantMap lastPlayedGame() const;
    // Remember where the viewer stopped in a playlist (index 0 clears it).
    Q_INVOKABLE void recordGamePosition(const QString &playlistPath, int index);
    // That remembered index, or -1.
    Q_INVOKABLE int gamePosition(const QString &playlistPath) const;

    // System id derived from a playlist path: its filename without extension,
    // lowercased. "Sega - Mega Drive - Genesis.lpl" -> "sega - mega drive - genesis".
    Q_INVOKABLE QString systemFromPlaylist(const QString &playlistPath) const;

    // Human-readable short name for a system id (e.g. "sega - mega drive - genesis" -> "GENESIS").
    Q_INVOKABLE QString shortNameForSystem(const QString &systemId) const;

signals:
    void systemsReady(const QVariantList &systems);
    void playlistsReady(const QVariantList &playlists);
    void gamesReady(const QVariantList &games);
    void retroarchPathChanged();
    void gameRunningChanged();
    // Emitted after an async prewarm manifest is installed, so a view that
    // already resolved a row can refresh it against the now-warm cache.
    void thumbnailManifestReady();
    // Emitted once the startup count-cache warm-up has finished merging into
    // m_playlistCountCache. Views never need it; the lab CTest waits on it.
    void playlistCountCacheWarmed();

public slots:
    // Called directly by AppCore::save_setting before moduleSettingChanged is
    // emitted. The old connection-based delivery ran after the views refreshed
    // and left their sort headers showing the previous value.
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value);

private:
    QString m_appRoot;
    QString m_dataRoot;
    QString m_playlistDir;
    QString m_romDir;
    QString m_thumbnailDir;
    QString m_retroarchPath;
    QString m_playlistSort;
    QString m_gameSort = QStringLiteral("A-Z");
    DisplayHandoff *m_handoff = nullptr;
    // Persisted recently-played / playlist-position bookkeeping. Loaded on
    // construction so a view can ask synchronously; written on launch and on
    // leaving a games list.
    RecentGamesStore m_recent;
    QProcess *m_gameProcess = nullptr; // the live launchGame() child, null when not running

    // PERF-TRANSITION-01 lab A/B: refreshAllPlaylists() re-parses every game
    // of every playlist on each view rebuild just for the count. Cache the
    // per-file entry keyed by mtime+size so unchanged files skip the parse.
    struct PlaylistCountCache { qint64 mtime; qint64 size; QVariantMap entry; };
    QHash<QString, PlaylistCountCache> m_playlistCountCache;
    QMutex m_countCacheMutex;
    QThread *m_countWarmThread = nullptr;
    // Bumped when the playlist directory changes, so an in-flight warm-up
    // cannot merge entries for the old directory into a cleared cache.
    quint64 m_playlistDirGeneration = 0;

    // RETRO-THUMB-CACHE: one mtime+size-invalidated basename manifest per
    // thumbnail type dir, LRU-bounded by directory count. mutable because
    // resolveThumbnail()/findThumbnail() are const QML-facing calls.
    mutable retro::ThumbnailManifestCache m_thumbManifestCache;

    // RETRO-THUMB-ASYNC: the first manifest build is a ~250 ms directory
    // listing at 50k files. Prewarm it on a worker thread at startup and on
    // thumbnail_directory change so the first QML resolve is a hash hit.
    // `m_thumbPrewarmContext` lives in the worker thread; all cache mutation
    // is posted back to this object's thread and generation-checked there.
    QThread m_thumbPrewarmThread;
    QObject *m_thumbPrewarmContext = nullptr;

    void loadSettings();
    // Shared cache path for the GUI refresh and the startup warm-up: return
    // the entry for a playlist, parsing only when its mtime+size is not cached.
    QVariantMap playlistCountEntry(const QFileInfo &pl, const QString &systemId);
    QVariantMap makePlaylistCountEntry(const QFileInfo &pl, const QString &systemId, int count) const;
    // Runs once on a QThread started by the constructor. Parses the same
    // playlists refreshAllPlaylists() would, off the GUI thread, then merges
    // into the cache under lock so the first user-visible open is a cache hit.
    void warmPlaylistCountCache();
    // Playlist file filters: modern RetroArch writes <core>.json, older builds
    // .lpl. Both are scanned so either install works without configuration.
    static QStringList playlistFilters();
    QVariantList parsePlaylist(const QString &path, const QString &systemId);
    QString findThumbnail(const QString &gameName, const QString &dirName) const;
    // Shallow two-level scan of <thumbnails>/<playlist>/<type>; cheap even with
    // 50k files because only directories are listed. Seed list for prewarm.
    QStringList discoverThumbnailTypeDirs() const;
    // Queue one async build per existing type dir with the current generation.
    void prewarmThumbnailManifests();
    QString sanitizeForThumbnailDb(const QString &label) const;
    QString resolveRomPath(const QString &playlistPath, const QString &romPath) const;
    // Core .so location for an item: its core_path if present, else the cores
    // directory next to the config (where RetroArch installs cores).
    QString resolveCorePath(const QString &corePath, const QString &defaultCorePath) const;
};
