// Verifies the app-wide volume owner (issue #34): clamping, the
// amplifier-style mute rule, change notifications, and persistence.
// A fake in-memory mixer is injected so the test never touches real audio
// hardware (CI, WSL and the Pi all run the same assertions).
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVector>

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

// QObject fake carrying the async backends' reconciliation seam: mutations
// are accepted optimistically (like ALSA/PipeWire on the process seam) and
// hardware truth arrives later via stateChanged(), driven explicitly by the
// test. Records the ordered level requests so ordering can be asserted.
class ReconcilingFakeMixer : public QObject, public audio::VolumeMixer {
    Q_OBJECT
public:
    bool setLevel(int percent) override {
        m_level = qBound(0, percent, 100);
        levelRequests.append(m_level);
        return true;
    }
    int level() const override { return m_level; }
    bool isMuted() const override { return m_muted; }
    bool setMuted(bool muted) override { m_muted = muted; return true; }
    QString backendName() const override { return QStringLiteral("reconciling-fake"); }
    // Hardware reports back: rounding, external change, or failure rollback.
    void confirm(int level, bool muted) { emit stateChanged(level, muted); }

    QVector<int> levelRequests;
signals:
    void stateChanged(int level, bool muted);
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
            // Writes are debounced off the UI thread (t_82bed393); the
            // destructor flush guarantees the file before the next instance.
        }
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("volume.json"))));

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

    // --- t_82bed393: decoupled key path, reconciliation, debounced persist ---

    // The optimistic update is visible immediately — the key path never waits
    // for hardware — and a later confirmation of the same value adds no OSD
    // update (no flicker).
    void testOptimisticUpdateBeforeHardwareConfirm() {
        auto fake = std::make_unique<ReconcilingFakeMixer>();
        ReconcilingFakeMixer *raw = fake.get();
        VolumeManager vm(QString(), std::move(fake));
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);

        vm.setLevel(60);
        QCOMPARE(vm.level(), 60);            // immediate, no hardware wait
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toInt(), 60);
        QCOMPARE(raw->levelRequests.size(), 1);

        raw->confirm(60, false);             // hardware agrees, later
        QCOMPARE(spy.count(), 1);            // confirmation is not a new update

        vm.setMuted(true);
        QCOMPARE(spy.count(), 2);
        raw->confirm(60, true);
        QCOMPARE(spy.count(), 2);
    }

    // Hardware rounding (request 50, card reports 45) reconciles the UI and
    // the persisted state instead of stranding them apart.
    void testReconcilesHardwareRounding() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto fake = std::make_unique<ReconcilingFakeMixer>();
        ReconcilingFakeMixer *raw = fake.get();
        VolumeManager vm(dir.path(), std::move(fake));
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);

        vm.setLevel(50);
        raw->confirm(45, false);
        QCOMPARE(vm.level(), 45);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last().at(0).toInt(), 45);

        QVERIFY(vm.persist());
        QFile f(dir.filePath(QStringLiteral("volume.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("level")).toInt(), 45);
    }

    // A failing backend rolls back to confirmed state; the UI follows and the
    // persisted state converges — no permanent inconsistency.
    void testFailureRollbackReconcilesUiAndPersist() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto fake = std::make_unique<ReconcilingFakeMixer>();
        ReconcilingFakeMixer *raw = fake.get();
        VolumeManager vm(dir.path(), std::move(fake));

        vm.setLevel(85);
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);
        raw->confirm(100, false);            // backend rollback after failure
        QCOMPARE(vm.level(), 100);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toInt(), 100);

        QVERIFY(vm.persist());
        QFile f(dir.filePath(QStringLiteral("volume.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("level")).toInt(), 100);
    }

    // The amplifier raise-while-muted is ONE coherent OSD update carrying the
    // final (level, mute) pair — never two paints for one press.
    void testRaiseWhileMutedIsOneOsdUpdate() {
        VolumeManager vm(QString(), std::make_unique<FakeMixer>());
        vm.setLevel(20);
        vm.setMuted(true);
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);

        vm.changeVolume(+VolumeManager::kStep);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toInt(), 25);
        QCOMPARE(spy.last().at(1).toBool(), false);
        QCOMPARE(vm.level(), 25);
        QVERIFY(!vm.muted());
    }

    // A burst applies in order, the last requested value wins, each effective
    // press emits exactly once, and the settled confirmation adds nothing.
    void testBurstAppliesInOrderLastWins() {
        auto fake = std::make_unique<ReconcilingFakeMixer>();
        ReconcilingFakeMixer *raw = fake.get();
        VolumeManager vm(QString(), std::move(fake));
        QSignalSpy spy(&vm, &VolumeManager::volumeChanged);

        for (int i = 0; i < 5; ++i)
            vm.changeVolume(-VolumeManager::kStep);
        QCOMPARE(vm.level(), 75);
        QCOMPARE(spy.count(), 5);
        QCOMPARE(spy.last().at(0).toInt(), 75);
        QCOMPARE(raw->levelRequests, QVector<int>({95, 90, 85, 80, 75}));

        raw->confirm(75, false);
        QCOMPARE(spy.count(), 5);
    }

    // Key repeat coalesces to ONE debounced write off the UI thread: nothing
    // on disk mid-burst, the final value on disk after the window.
    void testDebouncedPersistenceCoalescesBurst() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("volume.json"));
        VolumeManager vm(dir.path(), std::make_unique<FakeMixer>());
        for (int i = 0; i < 10; ++i)
            vm.changeVolume(-VolumeManager::kStep);
        QCOMPARE(vm.level(), 50);
        // No write per event: the debounce has not fired (no events pumped).
        QVERIFY(!QFile::exists(path));

        QTest::qWait(VolumeManager::kPersistDebounceMs + 150);
        QElapsedTimer t;
        t.start();
        while (!QFile::exists(path) && t.elapsed() < 2000)
            QTest::qWait(10);
        QVERIFY(QFile::exists(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("level")).toInt(), 50);
        QCOMPARE(o.value(QStringLiteral("muted")).toBool(), false);
    }

    // Destruction flushes a still-pending debounce synchronously: a restart
    // always restores the last state even if the window never elapsed.
    void testDestructorFlushesPendingWrite() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("volume.json"));
        {
            VolumeManager vm(dir.path(), std::make_unique<FakeMixer>());
            vm.changeVolume(-VolumeManager::kStep);
            vm.changeVolume(-VolumeManager::kStep);
            QVERIFY(!QFile::exists(path));
        }
        QVERIFY(QFile::exists(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("level")).toInt(), 90);
    }

    // Restore applies the persisted state without rewriting the file: startup
    // adds no write amplification, and the debounce stays idle.
    void testRestoreDoesNotRewriteDisk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("volume.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"level\":30,\"muted\":true}");
        f.close();
        const QDateTime mtime = QFileInfo(path).lastModified();
        {
            VolumeManager vm(dir.path(), std::make_unique<FakeMixer>());
            QCOMPARE(vm.level(), 30);
            QVERIFY(vm.muted());
            QTest::qWait(VolumeManager::kPersistDebounceMs + 150);
            QCOMPARE(QFileInfo(path).lastModified(), mtime);
        }
        QCOMPARE(QFileInfo(path).lastModified(), mtime);
    }
};

QTEST_GUILESS_MAIN(VolumeManagerTest)
#include "VolumeManagerTest.moc"
