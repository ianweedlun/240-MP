// Unit tests for the Plex module's resolution-aware direct-play / transcode
// decision. Pure logic — no network, no file I/O, no display.
#include <QtTest/QtTest>
#include "../PlaybackDecision.h"

class PlaybackDecisionTest : public QObject {
    Q_OBJECT

private slots:
    void alwaysDirectPlay() {
        // "auto" (Direct Play always): nothing ever transcodes, even 4K.
        QCOMPARE(PlexQuality::shouldTranscode("auto", 3840, 2160), false);
        QCOMPARE(PlexQuality::shouldTranscode("", 4096, 2160), false);
    }

    void atOrBelowThresholdDirectPlays() {
        // Exactly at the class threshold direct-plays (inclusive bound).
        QCOMPARE(PlexQuality::shouldTranscode("8000", 1920, 1080), false);
        QCOMPARE(PlexQuality::shouldTranscode("3000", 1280, 720), false);
        // Below too.
        QCOMPARE(PlexQuality::shouldTranscode("8000", 1280, 720), false);
        QCOMPARE(PlexQuality::shouldTranscode("1500", 640, 480), false);
    }

    void aboveThresholdTranscodes() {
        // Just-above and far-above both transcode to the ladder target.
        QCOMPARE(PlexQuality::shouldTranscode("8000", 3840, 2160), true);
        QCOMPARE(PlexQuality::shouldTranscode("8000", 1921, 1081), true);
        QCOMPARE(PlexQuality::shouldTranscode("3000", 1920, 1080), true);
    }

    void portraitAndRotatedUseShortEdge() {
        // Resolution class is the SHORTER edge: a 1080x1920 portrait (or
        // rotation-metadata) file is 1080-class and direct-plays at 8000.
        QCOMPARE(PlexQuality::shouldTranscode("8000", 1080, 1920), false);
        QCOMPARE(PlexQuality::shouldTranscode("8000", 2160, 3840), true);
    }

    void unknownDimensionsTranscode() {
        // Missing stream metadata is the risky branch for a CRT — transcode.
        QCOMPARE(PlexQuality::shouldTranscode("8000", 0, 0), true);
        QCOMPARE(PlexQuality::shouldTranscode("8000", 1920, 0), true);
        // But an always-direct setting never transcodes regardless.
        QCOMPARE(PlexQuality::shouldTranscode("auto", 0, 0), false);
    }

    void legacyCustomBitrateAlwaysTranscodes() {
        // Ids not on our corrected ladder (including the pre-reorg 4000/2000
        // values from older config.json) keep the old semantics:
        // unconditionally transcode at that bitrate.
        QCOMPARE(PlexQuality::shouldTranscode("1200", 1920, 1080), true);
        QCOMPARE(PlexQuality::shouldTranscode("4000", 640, 480), true);
        QCOMPARE(PlexQuality::shouldTranscode("1200", 640, 480), true);
    }
};

QTEST_GUILESS_MAIN(PlaybackDecisionTest)
#include "PlaybackDecisionTest.moc"
