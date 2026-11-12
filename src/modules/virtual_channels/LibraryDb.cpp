#include "LibraryDb.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QVector>
#include <algorithm>

#ifdef APP_HAVE_SQLITE
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#endif

namespace vchan {

#ifdef APP_HAVE_SQLITE

namespace {

constexpr char SEP = '\x1f';   // separator inside a joined metadata column

QStringList splitList(const QString &joined) {
    QStringList out;
    for (const QString &part : joined.split(QChar(quint16(SEP)))) {
        const QString t = part.trimmed();
        if (!t.isEmpty()) out << t;
    }
    return out;
}

QString joinList(const QStringList &values) {
    QStringList clean;
    for (const QString &v : values) {
        const QString t = v.trimmed();
        if (!t.isEmpty()) clean << t;
    }
    return clean.join(QChar(quint16(SEP)));
}

QStringList unionLists(const QStringList &kept, const QStringList &incoming) {
    QStringList out = kept;
    QSet<QString> seen;
    for (const QString &v : kept) seen.insert(v.toLower());
    for (const QString &v : incoming) {
        const QString t = v.trimmed();
        if (t.isEmpty() || seen.contains(t.toLower())) continue;
        seen.insert(t.toLower());
        out << t;
    }
    return out;
}

QString dedupeKey(const QString &channel, const QString &title) {
    return channel + QLatin1Char('|') + title.toLower();
}

// Metadata column -> the subject tally it feeds. The row's own `channel` label
// decides which side of the dial a value is counted for, so a genre tallies
// under both sides and a studio only under the side the walk carried it on.
struct FieldMap { const char *column; const char *kind; };
const QVector<FieldMap> kFields {
    { "genres",      "genre"      },
    { "networks",    "network"    },
    { "studios",     "studio"     },
    { "collections", "collection" },
};

bool fieldIsKnown(const QString &field) {
    for (const FieldMap &f : kFields)
        if (field == QLatin1String(f.column)) return true;
    return false;
}

struct Row {
    QString dk, srcs, channel, title;
    QStringList genres, networks, studios, collections;
    qint64 updated = 0;
};

QStringList fieldsFor(const QString &field, const Row &r) {
    if (field == QLatin1String("genres")) return r.genres;
    if (field == QLatin1String("networks")) return r.networks;
    if (field == QLatin1String("studios")) return r.studios;
    return r.collections;
}

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const QString kCtxName { QStringLiteral("vchan-library-db") };

// Two facts about Debian's packaged Sql module drove this file's shape (both
// probed, not assumed):
//  * QSqlQuery(sql, db) + exec() runs the statement TWICE (the ctor executes and
//    exec() re-runs), so CREATE/INSERT statements error or double-write. Only
//    prepare() + exec() — and exec(sql) — run once. Everything here is written
//    prepare-then-exec.
//  * The SQLite path comes from setDatabaseName; the docs' setFileName is not in
//    the packaged headers on the Pi (Qt 6.8) nor dev/CI (Qt 6.10).
// SQL uses SQLite's own grammar (native UPDATE SET forms), never Qt's legacy
// dialect. Statements here are validated against the interpreter in the unit
// test.
bool ensureSchema(QSqlDatabase &db) {
    QSqlQuery probe(db);
    if (probe.prepare(QStringLiteral("SELECT 1 FROM items LIMIT 1")) && probe.exec())
        return true;   // an old index: the tables are already there
    const QVector<QString> stmts {
        QStringLiteral("CREATE TABLE items (dk TEXT PRIMARY KEY, srcs TEXT,"
                       " channel TEXT, title TEXT, genres TEXT, networks TEXT,"
                       " studios TEXT, collections TEXT, updated INTEGER)"),
        QStringLiteral("CREATE TABLE subjects (kind TEXT, value TEXT,"
                       " channel TEXT, count INTEGER)"),
        QStringLiteral("CREATE INDEX subjects_kx ON subjects (kind, channel)"),
        QStringLiteral("CREATE TABLE meta (name TEXT PRIMARY KEY, value TEXT)"),
    };
    for (const QString &s : stmts) {
        QSqlQuery q(db);
        if (!q.prepare(s) || !q.exec()) return false;
    }
    return true;
}

qint64 readRebuildStamp(QSqlDatabase &db) {
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("SELECT value FROM meta WHERE name = :n"))) return 0;
    q.bindValue(QStringLiteral(":n"), QStringLiteral("last_rebuild_ms"));
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toLongLong();
}

void writeRebuildStamp(QSqlDatabase &db, qint64 ms) {
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (name, value) VALUES (:n, :v)"))) return;
    q.bindValue(QStringLiteral(":n"), QStringLiteral("last_rebuild_ms"));
    q.bindValue(QStringLiteral(":v"), QString::number(ms));
    q.exec();   // a failed stamp is cosmetic: the next rebuild rewrites it
}

} // namespace

// ---- lifecycle -------------------------------------------------------------

bool LibraryDb::open(const QString &path) {
    if (path.isEmpty()) return false;
    if (m_open && m_path == path) return true;
    close();
    QSqlDatabase::removeDatabase(kCtxName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kCtxName);
    // setDatabaseName is the SQLite path setter both Debian Qt 6.8 (the Pi) and
    // Ubuntu Qt 6.10 (dev/CI) ship; the upstream docs' setFileName is absent
    // from the packaged headers on both.
    db.setDatabaseName(path);
    if (!db.open()) return false;
    if (!ensureSchema(db)) { db.close(); return false; }
    m_path = path;
    m_open = true;
    m_lastRebuildMs = readRebuildStamp(db);
    return true;
}

void LibraryDb::close() {
    if (!m_open) return;
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (db.isValid()) db.close();
    m_open = false;
}

// ---- the walk's merge + rebuild --------------------------------------------
// Existing rows are read first so a walk that never mentions a server leaves
// that server's rows standing (the plan's rule: keep the last-known lineup).
// Merging is by (side, title): two servers' rows for the same show collapse to
// one, and each metadata field is the UNION of what the servers said — except
// that a field the new walk left EMPTY keeps the old value, which is how a
// network resolved by TheTVDB survives walks that don't re-resolve it.

bool LibraryDb::mergeAndRebuild(const QVector<LibraryRecord> &records, QString *why) {
    if (!m_open) { if (why) *why = QStringLiteral("library db not open"); return false; }
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) { if (why) *why = QStringLiteral("library db handle lost"); return false; }

    QHash<QString, Row> rows;
    {
        QSqlQuery q(db);
        if (!q.prepare(QStringLiteral(
            "SELECT dk, srcs, channel, title, genres, networks, studios, collections, updated"
            " FROM items"))) { if (why) *why = QStringLiteral("read of items failed"); return false; }
        if (!q.exec()) { if (why) *why = QStringLiteral("read of items failed"); return false; }
        while (q.next()) {
            Row r;
            r.dk = q.value(0).toString();
            r.srcs = q.value(1).toString();
            r.channel = q.value(2).toString();
            r.title = q.value(3).toString();
            r.genres = splitList(q.value(4).toString());
            r.networks = splitList(q.value(5).toString());
            r.studios = splitList(q.value(6).toString());
            r.collections = splitList(q.value(7).toString());
            r.updated = q.value(8).toLongLong();
            rows.insert(r.dk, r);
        }
    }

    for (const LibraryRecord &rec : records) {
        if (rec.title.isEmpty()) continue;
        const QString channel = (rec.channel == QLatin1String("movies"))
                                    ? QStringLiteral("movies") : QStringLiteral("tv");
        const QString key = dedupeKey(channel, rec.title);
        Row &into = rows[key];   // absent key -> an empty row we then fill
        if (into.title.isEmpty()) {
            into.dk = key;
            into.channel = channel;
            into.title = rec.title;
        }
        into.srcs = joinList(unionLists(splitList(into.srcs), { rec.src }));
        into.genres = unionLists(into.genres, rec.genres);
        into.networks = rec.networks.isEmpty() ? into.networks
                                               : unionLists(into.networks, rec.networks);
        into.studios = rec.studios.isEmpty() ? into.studios
                                             : unionLists(into.studios, rec.studios);
        into.collections = rec.collections.isEmpty() ? into.collections
                                                     : unionLists(into.collections, rec.collections);
        into.updated = qMax(into.updated, rec.updatedMs);
    }

    if (!db.transaction()) { if (why) *why = QStringLiteral("db refused a transaction"); return false; }
    {
        QSqlQuery wipe(db);
        if (!wipe.prepare(QStringLiteral("DELETE FROM items")) || !wipe.exec()) {
            db.rollback(); if (why) *why = QStringLiteral("index write failed"); return false;
        }
        if (!wipe.prepare(QStringLiteral("DELETE FROM subjects")) || !wipe.exec()) {
            db.rollback(); if (why) *why = QStringLiteral("index write failed"); return false;
        }
    }

    for (const Row &r : rows) {
        QSqlQuery ins(db);
        if (!ins.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO items (dk, srcs, channel, title, genres, networks,"
            " studios, collections, updated) VALUES (:dk, :srcs, :channel, :title,"
            " :genres, :networks, :studios, :collections, :updated)"))) {
            db.rollback(); if (why) *why = QStringLiteral("item write failed"); return false;
        }
        ins.bindValue(QStringLiteral(":dk"), r.dk);
        ins.bindValue(QStringLiteral(":srcs"), r.srcs);
        ins.bindValue(QStringLiteral(":channel"), r.channel);
        ins.bindValue(QStringLiteral(":title"), r.title);
        ins.bindValue(QStringLiteral(":genres"), joinList(r.genres));
        ins.bindValue(QStringLiteral(":networks"), joinList(r.networks));
        ins.bindValue(QStringLiteral(":studios"), joinList(r.studios));
        ins.bindValue(QStringLiteral(":collections"), joinList(r.collections));
        ins.bindValue(QStringLiteral(":updated"), r.updated);
        if (!ins.exec()) { db.rollback(); if (why) *why = QStringLiteral("item write failed"); return false; }
    }

    // Tallies come off the merged rows, not off the walk: deduped titles are
    // what a channel can actually air, so counting a show that lives on two
    // servers once is the same rule the union uses. The index key is
    // kind/channel/lower(value), which is what makes "Comedy" and "comedy"
    // from two servers a single row at one count.
    struct TallyRow { QString kind, value, channel; int count = 0; };
    QVector<TallyRow> tallies;
    QHash<QString, int> tallyAt;   // kind SEP low(value) SEP channel -> index
    for (const Row &r : rows) {
        for (const FieldMap &f : kFields) {
            const QStringList vals = fieldsFor(QLatin1String(f.column), r);
            for (const QString &v : vals) {
                const QString low = v.toLower();
                const QString tkey = QLatin1String(f.kind) + SEP + low + SEP + r.channel;
                if (tallyAt.contains(tkey)) {
                    tallies[tallyAt.value(tkey)].count++;
                } else {
                    TallyRow tr;
                    tr.kind = QLatin1String(f.kind);
                    tr.value = v;         // first spelling seen wins
                    tr.channel = r.channel;
                    tr.count = 1;
                    tallyAt.insert(tkey, tallies.size());
                    tallies.append(tr);
                }
            }
        }
    }
    for (const TallyRow &t : tallies) {
        QSqlQuery sub(db);
        if (!sub.prepare(QStringLiteral("INSERT OR REPLACE INTO subjects (kind, value, channel, count) VALUES (:k, :v, :c, :n)"))) {
            db.rollback(); if (why) *why = QStringLiteral("tally write failed"); return false;
        }
        sub.bindValue(QStringLiteral(":k"), t.kind);
        sub.bindValue(QStringLiteral(":v"), t.value);
        sub.bindValue(QStringLiteral(":c"), t.channel);
        sub.bindValue(QStringLiteral(":n"), t.count);
        if (!sub.exec()) { db.rollback(); if (why) *why = QStringLiteral("tally write failed"); return false; }
    }
    writeRebuildStamp(db, nowMs());
    if (!db.commit()) { if (why) *why = QStringLiteral("tally commit failed"); return false; }
    m_lastRebuildMs = readRebuildStamp(db);
    return true;
}

// Same machinery, empty start: a walk that owns the whole truth (tests, and a
// from-scratch index) replaces rather than merges.
bool LibraryDb::replaceAll(const QVector<LibraryRecord> &records) {
    if (!m_open) return false;
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return false;
    if (!db.transaction()) return false;
    QSqlQuery wipe(db);
    if (!wipe.prepare(QStringLiteral("DELETE FROM items")) || !wipe.exec()) { db.rollback(); return false; }
    if (!wipe.prepare(QStringLiteral("DELETE FROM subjects")) || !wipe.exec()) { db.rollback(); return false; }
    if (!db.commit()) return false;
    return mergeAndRebuild(records);
}

// Counts the servers already know that no title row carries: a boxset row
// already says how many films a collection holds. Applied after a rebuild,
// because a rebuild recomputes the tallies from the title rows and would
// otherwise drop them. Across sources the largest count wins — never a total,
// because a collection sitting on two servers is still one shelf a viewer can
// fill.
bool LibraryDb::applyPresenceRows(const QVariantList &rows, QString *why) {
    if (!m_open) { if (why) *why = QStringLiteral("library db not open"); return false; }
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) { if (why) *why = QStringLiteral("library db handle lost"); return false; }
    if (!db.transaction()) { if (why) *why = QStringLiteral("db refused a transaction"); return false; }
    for (const QVariant &row : rows) {
        const QVariantMap m = row.toMap();
        const QString kind = m.value(QStringLiteral("kind")).toString();
        const QString value = m.value(QStringLiteral("value")).toString();
        const QString channel = m.value(QStringLiteral("channel")).toString();
        const int count = m.value(QStringLiteral("count")).toInt();
        if (kind.isEmpty() || value.isEmpty() || channel.isEmpty() || count <= 0) continue;
        // Two statements: place it, then raise the standing count to the larger
        // of the two — SQLite has no per-field max inside INSERT OR REPLACE,
        // and the CASE keeps whichever count a case-variant row already carried.
        QSqlQuery put(db);
        if (!put.prepare(QStringLiteral("INSERT INTO subjects (kind, value, channel, count) VALUES (:k, :v, :c, :n)"))) {
            db.rollback(); if (why) *why = QStringLiteral("collection row write failed"); return false;
        }
        put.bindValue(QStringLiteral(":k"), kind);
        put.bindValue(QStringLiteral(":v"), value);
        put.bindValue(QStringLiteral(":c"), channel);
        put.bindValue(QStringLiteral(":n"), count);
        if (!put.exec()) { db.rollback(); if (why) *why = QStringLiteral("collection row write failed"); return false; }
        QSqlQuery raise(db);
        if (!raise.prepare(QStringLiteral(
            "UPDATE subjects SET count = CASE WHEN count > :n THEN count ELSE :n END"
            " WHERE kind = :k AND channel = :c AND value COLLATE NOCASE = :v"))) {
            db.rollback(); if (why) *why = QStringLiteral("collection row raise failed"); return false;
        }
        raise.bindValue(QStringLiteral(":k"), kind);
        raise.bindValue(QStringLiteral(":c"), channel);
        raise.bindValue(QStringLiteral(":v"), value);
        raise.bindValue(QStringLiteral(":n"), count);
        if (!raise.exec()) { db.rollback(); if (why) *why = QStringLiteral("collection row raise failed"); return false; }
    }
    if (!db.commit()) { if (why) *why = QStringLiteral("collection rows commit failed"); return false; }
    return true;
}

// ---- answers ---------------------------------------------------------------

QVariantList LibraryDb::presence(const QString &subjectKind, const QString &channel,
                                 int limit) const {
    QVariantList out;
    if (!m_open || subjectKind.isEmpty()) return out;
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return out;
    // A channel names one side of the dial: the SQL does the ordering and the
    // limit. Empty channel pools both sides, so the merge runs here: two reads,
    // case-folded together, then one sort (a per-side ORDER BY cannot survive
    // summing across sides).
    if (!channel.isEmpty()) {
        QSqlQuery q(db);
        if (!q.prepare(QStringLiteral(
            "SELECT value, count FROM subjects"
            " WHERE kind = :k AND channel = :c"
            " ORDER BY count DESC, value COLLATE NOCASE LIMIT :l")))
            return out;
        q.bindValue(QStringLiteral(":k"), subjectKind);
        q.bindValue(QStringLiteral(":c"), channel);
        q.bindValue(QStringLiteral(":l"), limit);
        if (!q.exec()) return out;
        while (q.next())
            out.append(QVariantMap { { QStringLiteral("id"), q.value(0) },
                                     { QStringLiteral("label"), q.value(0) },
                                     { QStringLiteral("count"), q.value(1) } });
        return out;
    }
    struct Pool { QString display; int count = 0; };
    QHash<QString, Pool> pooled;
    QHash<QString, int> firstSeen;   // stable tie-break: walk order
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("SELECT value, count FROM subjects WHERE kind = :k")))
        return out;
    q.bindValue(QStringLiteral(":k"), subjectKind);
    if (!q.exec()) return out;
    int seq = 0;
    while (q.next()) {
        const QString v = q.value(0).toString();
        const int n = q.value(1).toInt();
        const QString low = v.toLower();
        if (pooled.contains(low)) {
            pooled[low].count += n;
        } else {
            Pool p { v, n };
            pooled.insert(low, p);
            firstSeen.insert(low, seq++);
        }
    }
    QStringList keys;
    for (auto it = pooled.cbegin(); it != pooled.cend(); ++it) keys << it.key();
    std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        const Pool &pa = pooled[a];
        const Pool &pb = pooled[b];
        if (pa.count != pb.count) return pa.count > pb.count;
        const int cmp = pa.display.compare(pb.display, Qt::CaseInsensitive);
        if (cmp != 0) return cmp < 0;
        return firstSeen[a] < firstSeen[b];
    });
    for (const QString &k : keys) {
        if (out.size() >= limit) break;
        const Pool &p = pooled[k];
        out.append(QVariantMap { { QStringLiteral("id"), p.display },
                                 { QStringLiteral("label"), p.display },
                                 { QStringLiteral("count"), p.count } });
    }
    return out;
}

int LibraryDb::countFor(const QString &subjectKind, const QString &value,
                        const QString &channel) const {
    if (!m_open || value.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return 0;
    // value COLLATE NOCASE = :v: the collation binds to the column side, which
    // is what makes the lookup answer for either spelling (the probe pinned the
    // other placement silently missing).
    QSqlQuery q(db);
    if (channel.isEmpty()) {
        if (!q.prepare(QStringLiteral("SELECT SUM(count) FROM subjects WHERE kind = :k AND value COLLATE NOCASE = :v")))
            return 0;
    } else {
        if (!q.prepare(QStringLiteral("SELECT SUM(count) FROM subjects WHERE kind = :k AND channel = :c AND value COLLATE NOCASE = :v")))
            return 0;
        q.bindValue(QStringLiteral(":c"), channel);
    }
    q.bindValue(QStringLiteral(":k"), subjectKind);
    q.bindValue(QStringLiteral(":v"), value);
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toInt();
}

QStringList LibraryDb::metadataFor(const QString &title, const QString &channel,
                                   const QString &field) const {
    if (!m_open || title.isEmpty() || field.isEmpty() || !fieldIsKnown(field)) return {};
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return {};
    // SQLite cannot name a COLUMN with a bound parameter (a bound value is
    // data), so the column comes from the closed kFields list above —
    // unknown names are refused above, which keeps the composed SQL static.
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("SELECT %1 FROM items WHERE dk = :dk").arg(field))) return {};
    q.bindValue(QStringLiteral(":dk"), dedupeKey(channel, title));
    if (!q.exec() || !q.next()) return {};
    return splitList(q.value(0).toString());
}

bool LibraryDb::noteMetadata(const QString &title, const QString &channel,
                             const QString &field, const QStringList &values) {
    if (!m_open || title.isEmpty() || field.isEmpty() || !fieldIsKnown(field)) return false;
    const QStringList existing = metadataFor(title, channel, field);
    const QStringList merged = unionLists(existing, values);
    if (merged == existing) return true;   // nothing new to say
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return false;
    // UPDATE with SQLite's own grammar (the packaged plugin parses SQLite, not
    // Qt's legacy dialect), and the column comes from the closed field list.
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("UPDATE items SET %1 = :vals WHERE dk = :dk").arg(field))) return false;
    q.bindValue(QStringLiteral(":vals"), joinList(merged));
    q.bindValue(QStringLiteral(":dk"), dedupeKey(channel, title));
    return q.exec();
}

bool LibraryDb::isEmpty() const {
    if (!m_open) return true;
    QSqlDatabase db = QSqlDatabase::database(kCtxName);
    if (!db.isValid()) return true;
    QSqlQuery q(db);
    if (!q.prepare(QStringLiteral("SELECT COUNT(*) AS n FROM items"))) return true;
    if (!q.exec() || !q.next()) return true;
    return q.value(0).toInt() == 0;
}

qint64 LibraryDb::lastRebuildMs() const { return m_lastRebuildMs; }

#else   // ---- compiled without Qt's Sql module: nothing this build can store

bool LibraryDb::open(const QString &) { return false; }
void LibraryDb::close() {}
bool LibraryDb::mergeAndRebuild(const QVector<LibraryRecord> &, QString *why) {
    if (why) *why = QStringLiteral("library db unavailable (built without Sql)");
    return false;
}
bool LibraryDb::applyPresenceRows(const QVariantList &, QString *why) {
    if (why) *why = QStringLiteral("library db unavailable (built without Sql)");
    return false;
}
bool LibraryDb::replaceAll(const QVector<LibraryRecord> &) { return false; }
QVariantList LibraryDb::presence(const QString &, const QString &, int) const { return {}; }
int LibraryDb::countFor(const QString &, const QString &, const QString &) const { return 0; }
QStringList LibraryDb::metadataFor(const QString &, const QString &, const QString &) const { return {}; }
bool LibraryDb::noteMetadata(const QString &, const QString &, const QString &, const QStringList &) { return false; }
bool LibraryDb::isEmpty() const { return true; }
qint64 LibraryDb::lastRebuildMs() const { return 0; }

#endif   // APP_HAVE_SQLITE

}   // namespace vchan
