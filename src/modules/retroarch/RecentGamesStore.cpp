#include "RecentGamesStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
// The fields a "recent" row needs to launch a game and label it, and nothing
// else: thumbnail paths are resolved on demand and romPath re-rooting happens
// at parse time, so storing either would go stale when folders move.
const char *const kEntryKeys[] = {
    "name", "romPath", "corePath", "coreName",
    "playlistPath", "playlistBasename", "systemId", "shortName", "lastPlayed"
};
} // namespace

RecentGamesStore::RecentGamesStore(QString filePath)
    : m_filePath(std::move(filePath))
{
}

QVariantMap RecentGamesStore::lastPlayed() const
{
    return m_entries.isEmpty() ? QVariantMap{} : m_entries.first().toMap();
}

int RecentGamesStore::position(const QString &playlistPath) const
{
    if (playlistPath.isEmpty())
        return -1;
    const auto it = m_positions.constFind(playlistPath);
    if (it == m_positions.constEnd())
        return -1;
    bool ok = false;
    const int idx = it.value().toInt(&ok);
    return (ok && idx > 0) ? idx : -1;
}

QString RecentGamesStore::identityOf(const QVariantMap &game)
{
    const QString rom = game.value(QStringLiteral("romPath")).toString();
    if (!rom.isEmpty())
        return rom;
    // Items with no path (malformed playlists, hand-edited files) still deserve
    // one slot rather than a duplicate per launch.
    return game.value(QStringLiteral("playlistPath")).toString()
         + QLatin1Char('\x1f')
         + game.value(QStringLiteral("name")).toString();
}

void RecentGamesStore::recordLaunch(const QVariantMap &game, qint64 epochSeconds)
{
    const QString id = identityOf(game);
    if (id.isEmpty())
        return;

    QVariantMap entry;
    for (const char *key : kEntryKeys) {
        const QString k = QString::fromLatin1(key);
        if (k == QLatin1String("lastPlayed"))
            entry.insert(k, epochSeconds);
        else if (k == QLatin1String("playlistPath") || k == QLatin1String("playlistBasename")) {
            // Both come from the caller's navParams; Games.qml passes them on
            // the game map, so they survive the round trip through JSON.
            entry.insert(k, game.value(k));
        } else {
            entry.insert(k, game.value(k));
        }
    }
    // `thumbnail` is deliberately dropped: it is recomputed per view from the
    // playlist basename, and a cached absolute path would rot.
    entry.remove(QStringLiteral("thumbnail"));

    QVariantList kept;
    kept.reserve(qMin(m_entries.size(), kMaxEntries - 1) + 1);
    kept.append(entry);
    for (const QVariant &existing : m_entries) {
        if (kept.size() >= kMaxEntries)
            break;
        if (identityOf(existing.toMap()) == id)
            continue;                      // re-launch: the fresh copy wins
        kept.append(existing);
    }
    m_entries = kept;
}

void RecentGamesStore::recordPosition(const QString &playlistPath, int index)
{
    if (playlistPath.isEmpty())
        return;
    if (index <= 0) {
        m_positions.remove(playlistPath);  // top of a list is the default state
        return;
    }
    m_positions.insert(playlistPath, index);
}

bool RecentGamesStore::load()
{
    m_entries.clear();
    m_positions.clear();

    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;                      // never played anything yet: normal

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;                      // corrupt: behaves exactly like absent

    fromJson(doc.object().toVariantMap());
    return true;
}

bool RecentGamesStore::save() const
{
    QSaveFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(QJsonObject::fromVariantMap(toJson())).toJson(QJsonDocument::Indented));
    return f.commit();
}

QVariantMap RecentGamesStore::toJson() const
{
    QVariantMap root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("entries"), m_entries);
    root.insert(QStringLiteral("positions"), m_positions);
    return root;
}

void RecentGamesStore::fromJson(const QVariantMap &root)
{
    const QVariantList raw = root.value(QStringLiteral("entries")).toList();
    for (const QVariant &v : raw) {
        if (m_entries.size() >= kMaxEntries)
            break;
        const QVariantMap e = v.toMap();
        if (e.value(QStringLiteral("name")).toString().isEmpty())
            continue;                      // skip junk, keep the rest
        m_entries.append(e);
    }
    // Drop exact duplicates that a hand-edited file could contain, keeping the
    // newest occurrence (the file is written newest-first).
    QVariantList deduped;
    QStringList seen;
    for (const QVariant &v : m_entries) {
        const QString id = identityOf(v.toMap());
        if (seen.contains(id))
            continue;
        seen.append(id);
        deduped.append(v);
    }
    m_entries = deduped;

    m_positions = root.value(QStringLiteral("positions")).toMap();
}
