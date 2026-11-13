#ifndef LIBRARYDB_H
#define LIBRARYDB_H

#include <QVariant>
#include <QVector>
#include <QString>
#include <QStringList>

#include <QDateTime>

namespace vchan {

// One library item as the walk sees it: what a title is on which side of the
// dial, and what the server swears is in that library. Presence — which genres,
// networks, studios and collections the shelves can actually air — is the
// source of truth for the add-flow option lists, so the module stops guessing
// from TheTVDB catalogs and stops offering a genre whose only show lives on
// the other side of the dial.
struct LibraryRecord {
    QString src;                    // backend name: plex, jellyfin, emby, local
    QString channel;                // "tv" | "movies" (the library's own side)
    QString title;
    QString itemId;                 // server-side id (ratingKey / itemId) for drill-down
    QStringList genres;             // as the server spells them
    QStringList networks;           // empty when the server does not carry them
    QStringList studios;
    QStringList collections;        // server-side boxesets/franchises
    qint64 updatedMs = 0;
};

// The persistent index over those records. One SQLite file under the app's data
// directory, rebuilt by the launch walk: tallies per (kind, channel) plus the
// per-title metadata the servers do not carry (the networks TheTVDB resolve
// fills). Reading is cheap and always available; the walk that refreshes it
// owns the write.
class LibraryDb {
public:
    // Open the index at `path` (empty = unusable). Opening creates the tables
    // on first use, so a fresh app gets an empty index instead of an error.
    // The SQLite path comes from setDatabaseName; the docs' setFileName is not
    // in the packaged headers on the Pi (Qt 6.8) nor dev/CI (Qt 6.10).
    bool open(const QString &path);
    void close();

    // The walk's merge + rebuild: `records` are this launch's walk over the
    // servers that answered. Rows for servers that stayed offline keep their
    // last-known lineup (the plan's rule), so presence cannot dark-room a
    // channel because one server timed out. Tallies are recomputed from the
    // merged rows, never accumulated onto the old ones — a genre nobody has
    // any more stops being offered, it does not sit there at a stale count.
    // `why` (optional) receives the reason a write failed, for the caller's log.
    bool mergeAndRebuild(const QVector<LibraryRecord> &records, QString *why = nullptr);
    // Same machinery, empty start: a walk that owns the whole truth (tests, and
    // a from-scratch index) replaces rather than merges.
    bool replaceAll(const QVector<LibraryRecord> &records);

    // Counts the servers already know that no title row carries: a boxset row
    // already says how many films a collection holds. Applied after a rebuild,
    // because a rebuild recomputes the tallies from the title rows and would
    // otherwise drop them. Across sources the largest count wins — never a
    // total, because a collection sitting on two servers is still one shelf a
    // viewer can fill.
    bool applyPresenceRows(const QVariantList &rows, QString *why = nullptr);

    // Answers. `subjectKind` is "genre" | "network" | "studio" | "collection";
    // `channel` narrows to one side of the dial ("tv"/"movies") or empty = both
    // sides pooled.
    // presence() returns {id,label,count} maps, ordered most present first
    // (ties alphabetical): the add-flow lists and the manual browse want the
    // same view, and the count is what lets a dial prefer a rich category.
    QVariantList presence(const QString &subjectKind, const QString &channel,
                          int limit) const;
    // The count for one (kind, value, channel) triple; 0 = the shelves do not
    // have it. A missing DB also reads 0, which makes an un-walked app fall
    // back to the catalog path rather than inventing a presence list.
    int countFor(const QString &subjectKind, const QString &value,
                 const QString &channel) const;

    // Per-title metadata (the network/studio a title carries). Empty list =
    // unknown, which is what the resolve path falls back to; it never means
    // "this title has no network".
    QStringList metadataFor(const QString &title, const QString &channel,
                            const QString &field) const;
    // Add to one title's metadata (union, never replace): the resolve's answer
    // has to survive the next walk that does not re-resolve the title.
    bool noteMetadata(const QString &title, const QString &channel,
                      const QString &field, const QStringList &values);

    // Cached browse listing ({id, label, sub}) for one server ("Plex"/"Jellyfin"/"Emby")
    // and side ("tv"/"movies"), ordered alphabetically.
    QVariantList browseItems(const QString &src, const QString &channel) const;

    // Nothing indexed yet: presence() is empty, so the QML layer shows the
    // catalog list instead of a confident empty one. Distinguishing "empty
    // shelves" from "never walked" is what keeps a fresh app from offering
    // zero genres on every channel.
    bool isEmpty() const;

    // When the index was last rebuilt (0 = never). A rebuild within the
    // session is a walk that ran; an old stamp after a reboot is a server
    // that stayed offline.
    qint64 lastRebuildMs() const;

private:
    bool m_open = false;
    bool m_writable = false;
    QString m_path;
    qint64 m_lastRebuildMs = 0;
};

} // namespace vchan

#endif // LIBRARYDB_H
