#pragma once

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

namespace retro {

// Bounded, mtime+size-invalidated manifest of a thumbnail type directory.
//
// The shipped findThumbnail() relisted each type directory (and statted up to
// four candidate names) on EVERY resolveThumbnail() call. QML resolves one
// call per visible row, so walking a playlist paid N full directory listings
// of a tree that can hold tens of thousands of files. This cache keeps ONE
// basename->path index per absolute type-dir path; a warm resolve is a handful
// of hash lookups instead of a relist and a linear case-insensitive scan.
//
// Invalidation follows the PERF-TRANSITION-01 playlist-count cache: a cached
// manifest is reused only while the directory's (mtime, size) is unchanged.
// Eviction is capacity-only LRU over DIRECTORY PATHS (t_38e26883 policy): it
// never drops files from a manifest, because a match cap would drop legitimate
// thumbnails and change browse results. Metadata only — no image bytes are
// read and no file is moved.
//
// A pointer returned by manifest()/resolvePath() is owned by the cache and is
// valid until the next insert or clear(). Callers use it within one lookup.
class ThumbnailManifestCache {
public:
    struct Manifest {
        qint64 mtime = 0;
        qint64 size = 0;
        // False only when the dir was missing at build time; such a build is
        // never installed so a dir created later is still seen on first use.
        bool valid = false;
        // case-sensitive basename -> absolute path; ".png" wins over ".jpg"
        QHash<QString, QString> exact;
        // case-folded basename -> absolute path; first listing match wins
        QHash<QString, QString> folded;
    };

    explicit ThumbnailManifestCache(int maxDirectories = 256)
        : m_maxDirectories(maxDirectories > 0 ? maxDirectories : 1) {}

    // (mtime, size) of a directory. Kept as the single sampling point so tests
    // can drive mtime-only and size-only invalidation deterministically.
    using Key = QPair<qint64, qint64>;
    using KeyProvider = std::function<Key(const QString &dirPath)>;

    static Key defaultKey(const QString &dirPath) {
        const QFileInfo di(dirPath);
        return qMakePair(di.lastModified().toMSecsSinceEpoch(), di.size());
    }

    // Production never calls this; tests override the clock/stat with fixed
    // values. Passing an empty provider restores defaultKey().
    void setKeyProvider(KeyProvider provider) {
        m_keyProvider = std::move(provider);
    }

    // Current (mtime, size) key for dirPath. Production uses the real stat;
    // tests inject a deterministic clock. An async build captures this on the
    // GUI thread so install() can reject a key that changed while it ran.
    Key keyFor(const QString &dirPath) const {
        return m_keyProvider ? m_keyProvider(dirPath) : defaultKey(dirPath);
    }

    // Pure directory scan — never touches cache state, so it is safe to run
    // on a worker thread. `key` is sampled by the caller and stamped on the
    // result; the LRU/validity install stays on the GUI thread.
    static Manifest buildManifest(const QString &dirPath, const Key &key) {
        Manifest fresh;
        fresh.mtime = key.first;
        fresh.size = key.second;
        if (!QFileInfo(dirPath).isDir())
            return fresh;              // valid stays false — never installed

        fresh.valid = true;
        const QFileInfoList files = QDir(dirPath).entryInfoList(
            { QStringLiteral("*.png"), QStringLiteral("*.jpg") }, QDir::Files);
        for (const QFileInfo &file : files) {
            const QString base = file.completeBaseName();
            const QString path = file.absoluteFilePath();
            const bool isPng = file.suffix().compare(QLatin1String("png"),
                                                     Qt::CaseInsensitive) == 0;

            // Exact tier: the shipped code tried <name>.png before <name>.jpg,
            // so keep the .png preference when both extensions are present.
            auto exactIt = fresh.exact.constFind(base);
            if (exactIt == fresh.exact.constEnd()
                || (isPng && exactIt.value().endsWith(QLatin1String(".jpg"),
                                                      Qt::CaseInsensitive))) {
                fresh.exact.insert(base, path);
            }

            // Folded tier: the shipped code returned the FIRST entryInfoList
            // match. Check-before-insert preserves that tie-break; never let a
            // later colliding file overwrite an earlier one.
            const QString folded = base.toLower();
            if (!fresh.folded.contains(folded))
                fresh.folded.insert(folded, path);
        }
        return fresh;
    }

    // GUI-thread-only install of an async-built manifest. Stale work is
    // dropped: a generation bump (clear / thumbnail_directory change) or a
    // changed directory key means the index no longer describes the dir and
    // must not replace a fresher one.
    bool installPrewarmed(const QString &dirPath, const Key &key,
                          quint64 generation, const Manifest &built) {
        if (!built.valid || generation != m_generation)
            return false;
        if (built.mtime != key.first || built.size != key.second)
            return false;
        const Key now = keyFor(dirPath);
        if (now.first != key.first || now.second != key.second)
            return false;
        ++m_scans;                     // one real listing per installed build
        ++m_misses;
        insertManifest(dirPath, built);
        return true;
    }

    // Bumped by every clear() so in-flight async jobs can be discarded.
    quint64 generation() const { return m_generation; }

    // Return the manifest for dirPath, rebuilding ONLY when (mtime, size)
    // changed. A hit makes no entryInfoList() call. A missing/unreadable dir
    // returns nullptr and is not cached, so a dir created later is seen.
    const Manifest *manifest(const QString &dirPath) {
        const Key key = keyFor(dirPath);

        auto it = m_cache.constFind(dirPath);
        if (it != m_cache.constEnd()
            && it->mtime == key.first && it->size == key.second) {
            touch(dirPath);           // recency is touched on hits too
            ++m_hits;
            return &(*it);
        }

        if (!QFileInfo(dirPath).isDir())
            return nullptr;

        ++m_scans;                    // exactly one real listing per rebuild
        Manifest fresh = buildManifest(dirPath, key);
        insertManifest(dirPath, fresh);
        ++m_misses;
        auto cached = m_cache.constFind(dirPath);
        return &(*cached);
    }

    // Mirror of findThumbnail's three tiers: exact name, sanitized name, then
    // case-insensitive on both. name/sanitized are passed in so the backend
    // stays the single owner of its label-mangling rule.
    QString resolve(const QString &dirPath, const QString &name,
                    const QString &sanitized) {
        const Manifest *m = manifest(dirPath);
        if (!m)
            return QString();

        if (m->exact.contains(name))
            return m->exact.value(name);
        if (m->exact.contains(sanitized))
            return m->exact.value(sanitized);

        const QString foldedName = name.toLower();
        if (m->folded.contains(foldedName))
            return m->folded.value(foldedName);
        const QString foldedSanitized = sanitized.toLower();
        if (m->folded.contains(foldedSanitized))
            return m->folded.value(foldedSanitized);
        return QString();
    }

    void clear() {
        m_cache.clear();
        m_lru.clear();
        m_scans = 0;
        m_hits = 0;
        m_misses = 0;
        ++m_generation;              // invalidate any in-flight async builds
    }

    int size() const { return m_cache.size(); }
    int maxDirectories() const { return m_maxDirectories; }
    int scanCount() const { return m_scans; }
    int hitCount() const { return m_hits; }
    int missCount() const { return m_misses; }

private:
    void touch(const QString &key) {
        m_lru.removeAll(key);         // MRU front, LRU back
        m_lru.prepend(key);
    }

    void insertManifest(const QString &key, const Manifest &value) {
        m_cache.insert(key, value);
        touch(key);
        while (m_lru.size() > m_maxDirectories) {
            const QString victim = m_lru.takeLast();
            m_cache.remove(victim);
        }
    }

    int m_maxDirectories;
    QHash<QString, Manifest> m_cache;
    QStringList m_lru;
    int m_scans = 0;
    int m_hits = 0;
    int m_misses = 0;
    KeyProvider m_keyProvider;
    quint64 m_generation = 0;
};

} // namespace retro
