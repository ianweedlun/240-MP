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
        // On WSL/CI neither wpctl nor amixer exists, so we get the no-op.
        QCOMPARE(mixer->backendName(), QStringLiteral("none"));
    }

    // The factory's choice is stable across repeated calls.
    void testFactoryDeterministic() {
        QCOMPARE(makeSystemVolumeMixer()->backendName(),
                 makeSystemVolumeMixer()->backendName());
    }
};

QTEST_MAIN(VolumeMixerTest)
#include "VolumeMixerTest.moc"
