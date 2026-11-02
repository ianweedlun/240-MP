#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QProcess>

class DisplayHandoff;

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

public:
    explicit RetroArchBackend(const QString &appRoot, const QString &dataRoot,
                              DisplayHandoff *handoff, QObject *parent = nullptr);
    ~RetroArchBackend() override;

    QString retroarchPath() const { return m_retroarchPath; }

    // Current playlist sort mode (SYSTEM / ALPHA / COMPANY).
    QString playlistSort() const;

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
    // Launch a game: hand the display to RetroArch, run it with the item's
    // core, restore the display when it exits.
    Q_INVOKABLE void launchGame(const QVariantMap &game, const QString &systemId);

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

public slots:
    // Auto-wired by AppCore::registerBackend to appCore.moduleSettingChanged.
    // Without this, config changes (playlist sort) only reach us at startup,
    // and every refresh keeps sorting with the value from boot.
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value);

private:
    QString m_appRoot;
    QString m_dataRoot;
    QString m_playlistDir;
    QString m_romDir;
    QString m_thumbnailDir;
    QString m_retroarchPath;
    QString m_playlistSort;
    DisplayHandoff *m_handoff = nullptr;
    QProcess *m_gameProcess = nullptr; // the live launchGame() child, null when not running

    void loadSettings();
    // Playlist file filters: modern RetroArch writes <core>.json, older builds
    // .lpl. Both are scanned so either install works without configuration.
    static QStringList playlistFilters();
    QVariantList parsePlaylist(const QString &path, const QString &systemId);
    QString findThumbnail(const QString &gameName, const QString &dirName) const;
    QString sanitizeForThumbnailDb(const QString &label) const;
    QString resolveRomPath(const QString &playlistPath, const QString &romPath) const;
    // Core .so location for an item: its core_path if present, else the cores
    // directory next to the config (where RetroArch installs cores).
    QString resolveCorePath(const QString &corePath, const QString &defaultCorePath) const;
};
