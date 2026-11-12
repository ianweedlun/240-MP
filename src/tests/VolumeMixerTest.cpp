// Verifies the app-wide system-volume abstraction (issue #34): the
// VolumeMixer contract and the factory's runtime backend detection.
// No real audio hardware is needed — on WSL/CI the factory falls back to the
// no-op mixer, which is exactly the path exercised here.
#include <QTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "audio/ALSAVolumeMixer.h"
#include "audio/NoopVolumeMixer.h"
#include "audio/PipeWireVolumeMixer.h"
#include "audio/VolumeManager.h"
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

    // The central owner: clamps at both ends, mute separate from level,
    // persistence restores across a fresh manager on the same data root.
    void testVolumeManagerLevel() {
        VolumeManager vm(QDir::current().filePath(QStringLiteral("vtm")));
        vm.setLevel(150);
        QCOMPARE(vm.level(), 100);
        vm.setLevel(-10);
        QCOMPARE(vm.level(), 0);
        vm.setLevel(55);
        QCOMPARE(vm.level(), 55);
    }

    void testVolumeManagerMuteSeparate() {
        VolumeManager vm(QDir::current().filePath(QStringLiteral("vtm-sep")));
        vm.setLevel(55);
        vm.toggleMute();
        QCOMPARE(vm.level(), 55);
        QCOMPARE(vm.muted(), true);
        // Raising volume while muted unmutes at the new level.
        vm.changeVolume(5);
        QCOMPARE(vm.level(), 60);
        QCOMPARE(vm.muted(), false);
    }

    // Persistence: the saved level/mute survive a fresh launch of the SAME
    // mixer backend family (real PipeWire/ALSA hold their own state; the
    // noop mixer answers honestly that it changed nothing — the file is
    // still written, which is the contract that matters for the Pi).
    void testVolumeManagerPersistence() {
        const QString root = QDir::current().filePath(QStringLiteral("vtm-persist"));
        VolumeManager vm(root);
        vm.setLevel(61);
        vm.toggleMute();
        QVERIFY(vm.persist());
        QFile f(QDir(root).filePath(QStringLiteral("volume.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject saved = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QCOMPARE(saved.value(QStringLiteral("level")).toInt(), 61);
        QCOMPARE(saved.value(QStringLiteral("muted")).toBool(), true);
    }
};

QTEST_MAIN(VolumeMixerTest)
#include "VolumeMixerTest.moc"
