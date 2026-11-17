#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Recently played games, and where you were in each playlist.
//
// Kept out of RetroArchBackend so the rules are testable without a QObject, a
// RetroArch install or a display — this is bookkeeping, and bookkeeping is
// exactly the kind of logic that rots silently:
//
//  * A re-launch must MOVE the entry to the front, never add a second copy.
//    (Playing Super Metroid twice in an evening must not fill the list with it.)
//  * The list is capped, oldest dropped first.
//  * A corrupt or half-written file must read as "empty", never as a crash or a
//    half-populated list — this file lives on an SD card on a machine that gets
//    its power cut.
//
// Positions are stored per playlist path so returning to a 1746-game playlist
// lands where you left it, which in-session navListState alone cannot do across
// a service restart.
class RecentGamesStore
{
public:
    static constexpr int kMaxEntries = 12;

    explicit RecentGamesStore(QString filePath);

    // Newest first.
    const QVariantList &entries() const { return m_entries; }
    // Empty map when nothing has ever been played.
    QVariantMap lastPlayed() const;

    // Index to restore for a playlist, or -1 when this playlist was never left
    // part-way down.
    int position(const QString &playlistPath) const;

    // Record a launch: an entry is identified by its ROM path (label + playlist
    // as fallback, for the rare item with no path), so re-playing moves it to
    // the front instead of duplicating it.
    void recordLaunch(const QVariantMap &game, qint64 epochSeconds);

    // Remember where the viewer stopped in a playlist. Negative or out-of-range
    // values are clamped to 0, and index 0 is stored as "no position" so the top
    // of a list never costs a file write.
    void recordPosition(const QString &playlistPath, int index);

    bool load();
    // Atomic (QSaveFile): a power cut mid-write leaves the previous file intact
    // rather than a truncated one.
    bool save() const;

    QString filePath() const { return m_filePath; }

private:
    QVariantMap toJson() const;
    void fromJson(const QVariantMap &root);
    static QString identityOf(const QVariantMap &game);

    QString m_filePath;
    QVariantList m_entries;              // newest first
    QVariantMap m_positions;             // playlistPath -> index
};
