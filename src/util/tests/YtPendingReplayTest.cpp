#include <QtTest/QtTest>

#include "../YtPendingReplay.h"

// Regression around the OAuth-refresh replay markers (2026-11-18 youtube
// torture): a marker belongs to a DEFERRED request only. A normal, live
// request must not leave one behind, otherwise the next token swap replays
// stale work (list reload instead of the playlist the user opened) and
// re-fetches account lists after unrelated refreshes.
class YtPendingReplayTest : public QObject {
    Q_OBJECT

private slots:
    void liveListLoadLeavesNoStaleMarker();
    void deferredListReplaysExactlyOnce();
    void deferredVideosReplayExactlyOnce();
    void channelsIndependentOfList();
    void listOutranksVideosWhenBothDeferred();
    void declinedRefreshClearsEverything();
};

void YtPendingReplayTest::liveListLoadLeavesNoStaleMarker()
{
    ytui::PendingReplay pending;
    // The old call site marked EVERY list load as pending. A live feed load
    // followed by an expired playlist-videos request then replayed "feed"
    // instead of the playlist — blank playlist on the CRT.
    pending.deferList(QStringLiteral("feed"));
    pending.clearList(); // live request went out with a fresh bearer
    pending.deferPlaylistVideos(QStringLiteral("PLlife111111"));

    const ytui::PendingReplay::ListRequest req = pending.takeListOrVideos();
    QCOMPARE(req.kind, ytui::PendingReplay::ListRequest::Kind::PlaylistVideos);
    QCOMPARE(req.value, QStringLiteral("PLlife111111"));
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::None);
}

void YtPendingReplayTest::deferredListReplaysExactlyOnce()
{
    ytui::PendingReplay pending;
    pending.deferList(QStringLiteral("playlists"));
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::AccountList);
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::None);
}

void YtPendingReplayTest::deferredVideosReplayExactlyOnce()
{
    ytui::PendingReplay pending;
    pending.deferPlaylistVideos(QStringLiteral("LL"));
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::PlaylistVideos);
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::None);
}

void YtPendingReplayTest::channelsIndependentOfList()
{
    ytui::PendingReplay pending;
    pending.deferChannels();
    pending.deferList(QStringLiteral("history"));
    QVERIFY(pending.takeChannels());
    QVERIFY(!pending.takeChannels());
    const ytui::PendingReplay::ListRequest req = pending.takeListOrVideos();
    QCOMPARE(req.kind, ytui::PendingReplay::ListRequest::Kind::AccountList);
    QCOMPARE(req.value, QStringLiteral("history"));
}

void YtPendingReplayTest::listOutranksVideosWhenBothDeferred()
{
    ytui::PendingReplay pending;
    pending.deferPlaylistVideos(QStringLiteral("PLmix"));
    pending.deferList(QStringLiteral("watchlater"));
    const ytui::PendingReplay::ListRequest req = pending.takeListOrVideos();
    QCOMPARE(req.kind, ytui::PendingReplay::ListRequest::Kind::AccountList);
    QCOMPARE(req.value, QStringLiteral("watchlater"));
}

void YtPendingReplayTest::declinedRefreshClearsEverything()
{
    ytui::PendingReplay pending;
    pending.deferChannels();
    pending.deferList(QStringLiteral("feed"));
    pending.deferPlaylistVideos(QStringLiteral("PLx"));
    pending.clearAll();
    QVERIFY(!pending.takeChannels());
    QCOMPARE(pending.takeListOrVideos().kind,
             ytui::PendingReplay::ListRequest::Kind::None);
}

QTEST_MAIN(YtPendingReplayTest)
#include "YtPendingReplayTest.moc"
