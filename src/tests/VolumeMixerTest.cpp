// Verifies the app-wide system-volume abstraction (issue #34): the
// VolumeMixer contract and the factory's runtime backend detection.
// No real audio hardware is needed — on WSL/CI the factory falls back to the
// no-op mixer, which is exactly the path exercised here.
#include <QTest>

#include "audio/ALSAVolumeMixer.h"
#include "audio/NoopVolumeMixer.h"
#include "audio/PipeWireVolumeMixer.h"
#include "audio/VolumeMixerFactory.h"

using namespace audio;

class VolumeMixerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // setLevel clamps out-of-range values into 0..100.
    void testNoopClampsLevel() {
        NoopVolumeMixer m;
        QVERIFY(m.setLevel(150));
        QCOMPARE(m.level(), 100);
        QVERIFY(m.setLevel(-10));
        QCOMPARE(m.level(), 0);
        QCOMPARE(m.setLevel(42), true);
        QCOMPARE(m.level(), 42);
    }

    // Mute is tracked independently and survives level changes.
    void testNoopMuteState() {
        NoopVolumeMixer m;
        QVERIFY(!m.isMuted());
        QVERIFY(m.setMuted(true));
        QVERIFY(m.isMuted());
        // Turning the volume up is how you un-mute (matches a physical button).
        m.setLevel(80);
        QVERIFY(m.isMuted()); // no-op keeps whatever it was told
    }

    // Every backend reports a distinct, stable name.
    void testBackendNames() {
        QCOMPARE(NoopVolumeMixer().backendName(), QStringLiteral("none"));
        QCOMPARE(PipeWireVolumeMixer().backendName(), QStringLiteral("pipewire"));
        QCOMPARE(ALSAVolumeMixer().backendName(), QStringLiteral("alsa"));
    }

    // The factory always returns a usable mixer and never a null pointer.
    void testFactoryAlwaysSucceeds() {
        auto mixer = makeSystemVolumeMixer();
        QVERIFY(mixer != nullptr);
        // On machines without audio daemons (WSL/CI containers) it falls back to none;
        // on real hardware with ALSA or PipeWire, it selects that backend.
        const QString name = mixer->backendName();
        QVERIFY(name == QStringLiteral("none") ||
                name == QStringLiteral("alsa") ||
                name == QStringLiteral("pipewire"));
    }

    // The factory's choice is stable across repeated calls.
    void testFactoryDeterministic() {
        QCOMPARE(makeSystemVolumeMixer()->backendName(),
                 makeSystemVolumeMixer()->backendName());
    }

    // Auto-detection must land on a control that actually exists: a desktop's
    // 'Master', a Pi's 'PCM'/'Headphone', and a sane fallback when the card
    // exposes nothing we recognise (issue #34 — volume keys were a no-op on
    // hardware whose card has no 'Master').
    void testAlsaControlDetection() {
        // Pi bcm2835 (composite/headphone out).
        const QString pi = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(pi), QStringLiteral("PCM"));

        // Desktop card.
        const QString desktop = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Master',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(desktop), QStringLiteral("Master"));

        // Only a headphone control (some Pi configs).
        const QString phone = QStringLiteral("Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(phone), QStringLiteral("Headphone"));

        // Nothing usable -> historical default, not an arbitrary control.
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("Simple mixer control 'Mic',0\n")),
                 QStringLiteral("Master"));
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("")), QStringLiteral("Master"));
    }

    // amixer's `get` output carries the level as "[71%]" — the old regex looked
    // for a leading '%' and never matched, so level() always echoed its cache.
    void testAlsaLevelParsing() {
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Front Left: Playback 50 [71%] [-10.00dB] [on]\n"
                     "Front Right: Playback 50 [71%] [-10.00dB] [on]\n")), 71);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Mono: Playback 0 [0%] [-51.00dB] [off]")), 0);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Simple mixer control 'PCM',0")), -1);  // no level -> cache
    }
};

QTEST_MAIN(VolumeMixerTest)
#include "VolumeMixerTest.moc"
