// Verifies the app-wide volume owner (issue #34): clamping, the
// amplifier-style mute rule, change notifications, and persistence.
// A fake in-memory mixer is injected so the test never touches real audio
// hardware (CI, WSL and the Pi all run the same assertions).
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "audio/VolumeManager.h"
#include "audio/VolumeMixer.h"
#include "audio/VolumeKeyMap.h"

namespace {

class FakeMixer : public audio::VolumeMixer {
public:
    bool setLevel(int percent) override {
        m_level = qBound(0, percent, 100);
        ++setLevelCalls;
        return true;
    }
    int level() const override { return m_level; }
    bool isMuted() const override { return m_muted; }
    bool setMuted(bool muted) override {
        m_muted = muted;
        return true;
    }
    QString backendName() const override { return QStringLiteral("fake"); }

    int setLevelCalls = 0;

private:
    int m_level = 100;
    bool m_muted = false;
};

} // anonymous namespace

class VolumeManagerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // changeVolume clamps at both ends instead of wrapping or going negative.
    void testClampsAtBounds() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());

        vm.changeVolume(1000);
        QCOMPARE(vm.level(), 100);

        vm.changeVolume(-1000);
        QCOMPARE(vm.level(), 0);
    }

    // The OSD steps by kStep; three presses is three steps, not three points.
    void testStepsAccumulate() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(50);

        vm.changeVolume(VolumeManager::kStep);
        QCOMPARE(vm.level(), 50 + VolumeManager::kStep);
        vm.changeVolume(-VolumeManager::kStep);
        QCOMPARE(vm.level(), 50);
    }

    // A mute does not move the level, and unmuting restores the same number.
    void testMuteIsSeparateFromLevel() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(40);

        QVERIFY(vm.toggleMute());
        QVERIFY(vm.muted());
        QCOMPARE(vm.level(), 40);

        QVERIFY(vm.toggleMute());
        QVERIFY(!vm.muted());
        QCOMPARE(vm.level(), 40);
    }

    // Raising volume while muted unmutes at the new level (amplifier dial).
    void testRaiseWhileMutedUnmutes() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(20);
        vm.setMuted(true);

        vm.changeVolume(VolumeManager::kStep);

        QVERIFY(!vm.muted());
        QCOMPARE(vm.level(), 20 + VolumeManager::kStep);
    }

    // Lowering volume while muted stays muted: a quiet dial turn must not
    // surprise the room with sound.
    void testLowerWhileMutedStaysMuted() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(60);
        vm.setMuted(true);

        vm.changeVolume(-VolumeManager::kStep);

        QVERIFY(vm.muted());
        QCOMPARE(vm.level(), 60 - VolumeManager::kStep);
    }

    // Both renderers mirror this signal, so it must carry the settled values.
    void testEmitsSettledLevelAndMute() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(50);
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);

        vm.changeVolume(VolumeManager::kStep);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 50 + VolumeManager::kStep);
        QCOMPARE(spy.at(0).at(1).toBool(), false);

        vm.setMuted(true);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(1).toBool(), true);
    }

    // A restart must come back at the same level and mute state.
    void testPersistsAcrossInstances() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        {
            VolumeManager vm(dir.path(), std::make_unique<FakeMixer>());
            vm.setLevel(35);
            vm.setMuted(true);
            QVERIFY(QFile::exists(dir.filePath(QStringLiteral("volume.json"))));
        }

        VolumeManager restored(dir.path(), std::make_unique<FakeMixer>());
        QCOMPARE(restored.level(), 35);
        QCOMPARE(restored.muted(), true);
    }

    // A persisted mute survives the level restore that runs before it.
    void testRestoreKeepsMuteWhenLevelUnchanged() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            VolumeManager vm(dir.path(), std::make_unique<FakeMixer>());
            vm.setMuted(true);   // level stays at the fake's 100
        }

        VolumeManager restored(dir.path(), std::make_unique<FakeMixer>());
        QCOMPARE(restored.level(), 100);
        QCOMPARE(restored.muted(), true);
    }

    // No data root (tests, first run) must not crash or write anywhere.
    void testEmptyDataRootIsHarmless() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        QVERIFY(!vm.persist());
        QCOMPARE(vm.level(), 100);
        QVERIFY(!vm.muted());
    }

    // The backend name reaches the OSD status line unchanged.
    void testBackendNamePassthrough() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        QCOMPARE(vm.backendName(), QStringLiteral("fake"));
    }

    // ── The wired input path (#34) ────────────────────────────────────────────
    // InputManager hands VolumeManager the action this map reports, so the map
    // is the whole routing decision: the three consumer-control keys are the
    // only ones that move app-wide volume.
    void testVolumeKeyMapping() {
        QCOMPARE(audio::volumeKeyFor(Qt::Key_VolumeUp),   audio::VolumeKey::Up);
        QCOMPARE(audio::volumeKeyFor(Qt::Key_VolumeDown), audio::VolumeKey::Down);
        QCOMPARE(audio::volumeKeyFor(Qt::Key_VolumeMute), audio::VolumeKey::Mute);
        // Navigation keys must keep falling through to the menu, not touch audio.
        QCOMPARE(audio::volumeKeyFor(Qt::Key_Up),      audio::VolumeKey::None);
        QCOMPARE(audio::volumeKeyFor(Qt::Key_Down),    audio::VolumeKey::None);
        QCOMPARE(audio::volumeKeyFor(Qt::Key_Return),  audio::VolumeKey::None);
        QCOMPARE(audio::volumeKeyFor(Qt::Key_ChannelUp), audio::VolumeKey::None);
    }

    // Up/Down move by exactly one step — a held key repeats, so doubling the
    // step here would make the OSD bar race.
    void testApplyVolumeKeyStepsByKStep() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(50);

        audio::applyVolumeKey(vm, audio::VolumeKey::Up);
        QCOMPARE(vm.level(), 50 + VolumeManager::kStep);

        audio::applyVolumeKey(vm, audio::VolumeKey::Down);
        audio::applyVolumeKey(vm, audio::VolumeKey::Down);
        QCOMPARE(vm.level(), 50 - VolumeManager::kStep);
    }

    // Mute is the third key, and applying None must not disturb anything.
    void testApplyVolumeKeyMuteAndNone() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(30);

        audio::applyVolumeKey(vm, audio::VolumeKey::Mute);
        QVERIFY(vm.muted());
        QCOMPARE(vm.level(), 30);

        audio::applyVolumeKey(vm, audio::VolumeKey::None);
        QVERIFY(vm.muted());
        QCOMPARE(vm.level(), 30);
    }

    // Whatever the key path does, the mixer ends up at the manager's number:
    // that is what keeps the picture and the speaker from disagreeing.
    void testKeyPathReachesMixer() {
        auto fake = std::make_unique<FakeMixer>();
        FakeMixer *raw = fake.get();
        VolumeManager vm(QString(), std::move(fake));
        vm.setLevel(80);

        audio::applyVolumeKey(vm, audio::VolumeKey::Down);

        QCOMPARE(raw->level(), 80 - VolumeManager::kStep);
        QCOMPARE(vm.level(), raw->level());
    }
};

QTEST_GUILESS_MAIN(VolumeManagerTest)
#include "VolumeManagerTest.moc"
