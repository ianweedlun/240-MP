#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QProcess>

class DisplayHandoff;

// Browsing RetroArch's playlist library.
//
// The three tiers map to what RetroArch actually stores in
// ~/.config/retroarch/playlists/: each <core>.json file is one playlist of one
// system, and its items are games. Tier names come from the playlist filename
// ("snes9x_libretro.json" -> system "snes9x"), the playlist is the file itself,
// and the games are its items. Everything is read from local files — the only
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

    // Path getters for the settings UI.
    Q_INVOKABLE QString playlist_directory() const;
    Q_INVOKABLE QString rom_directory() const;
    Q_INVOKABLE QString thumbnail_directory() const;
    Q_INVOKABLE QString retroarch_path() const;

    // Scan the playlist directory and group its playlists by system. The list
    // is returned directly (a fast local scan — no round trip needed) and also
    // emitted, so a view can take whichever it finds convenient.
    Q_INVOKABLE QVariantList refreshSystems();
    // The playlists belonging to one system.
    Q_INVOKABLE QVariantList refreshPlaylists(const QString &systemId);
    // Parse one playlist file into its game entries.
    Q_INVOKABLE QVariantList refreshGames(const QString &playlistPath);
    // Launch a game: hand the display to RetroArch, run it with the item's
    // core, restore the display when it exits.
    Q_INVOKABLE void launchGame(const QVariantMap &game, const QString &systemId);

    // System id derived from a playlist path: its filename without extension,
    // lowercased. "snes9x_libretro.json" -> "snes9x_libretro".
    Q_INVOKABLE QString systemFromPlaylist(const QString &playlistPath) const;

signals:
    void systemsReady(const QVariantList &systems);
    void playlistsReady(const QVariantList &playlists);
    void gamesReady(const QVariantList &games);
    void retroarchPathChanged();

private:
    QString m_appRoot;
    QString m_dataRoot;
    QString m_playlistDir;
    QString m_romDir;
    QString m_thumbnailDir;
    QString m_retroarchPath;
    DisplayHandoff *m_handoff = nullptr;

    void loadSettings();
    // Playlist file filters: modern RetroArch writes <core>.json, older builds
    // .lpl. Both are scanned so either install works without configuration.
    static QStringList playlistFilters();
    QVariantList parsePlaylist(const QString &path, const QString &systemId);
    QString findThumbnail(const QString &gameName, const QString &systemId) const;
    QString resolveRomPath(const QString &playlistPath, const QString &romPath) const;
    // Core .so location for an item: its core_path if present, else the cores
    // directory next to the config (where RetroArch installs cores).
    QString resolveCorePath(const QString &corePath) const;
};
