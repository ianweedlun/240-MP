#pragma once

#include <QString>

namespace ytui {

// The account paths defer one InnerTube request behind an in-flight OAuth
// token swap. A marker must exist ONLY while a request is actually deferred:
// setting it on every call leaves a stale marker that the next unrelated
// refresh "replays" (a list reload instead of the requested playlist videos —
// blank playlist), and that fires spurious reloads after any token swap.
//
// Pure state machine: no network, no auth files, fully unit-testable.
class PendingReplay {
public:
    void deferList(const QString &kind)     { m_listKind = kind; }
    void deferPlaylistVideos(const QString &id) { m_videosId = id; }
    void deferChannels()                    { m_channels = true; }

    // A request that went out with a LIVE token is not deferred. Clear its
    // own marker so a later refresh cannot replay work that already ran.
    void clearList()     { m_listKind.clear(); }
    void clearVideos()   { m_videosId.clear(); }
    void clearChannels() { m_channels = false; }
    void clearAll()      { clearList(); clearVideos(); clearChannels(); }

    // One-shot: the success leg consumes each marker exactly once.
    bool takeChannels()
    {
        if (!m_channels)
            return false;
        m_channels = false;
        return true;
    }

    struct ListRequest {
        enum class Kind { None, AccountList, PlaylistVideos };
        Kind    kind = Kind::None;
        QString value;
    };

    // Existing precedence kept: an account list outranks playlist videos
    // when both were deferred.
    ListRequest takeListOrVideos()
    {
        if (!m_listKind.isEmpty()) {
            const QString kind = m_listKind;
            m_listKind.clear();
            return {ListRequest::Kind::AccountList, kind};
        }
        if (!m_videosId.isEmpty()) {
            const QString id = m_videosId;
            m_videosId.clear();
            return {ListRequest::Kind::PlaylistVideos, id};
        }
        return {};
    }

private:
    QString m_listKind;
    QString m_videosId;
    bool    m_channels = false;
};

} // namespace ytui
