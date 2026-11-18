// Regression for issue #85: AppCore::save_setting must deliver a module setting
// to backends BEFORE emitting moduleSettingChanged. QML connections refresh on
// that signal and immediately re-read the backend, so a late delivery leaves
// the first press looking like a no-op even though config.json changed.
//
// These tests use the REAL AppCore and RetroArchBackend (no QML stubs) so both
// halves of the contract are pinned: ordering, and the Q_INVOKABLE accessors
// the view needs. A stub listener stands in for the QML Connections handler by
// reading the backend from inside the AppCore signal callback.
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTest>
#include <QString>

#include "AppCore.h"
#include "modules/retroarch/RetroArchBackend.h"

namespace {
constexpr auto kRa = "com.240mp.retroarch";

void writeConfig(const QString &dir) {
    QFile f(dir + QStringLiteral("/config.json"));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"modules\":{\"com.240mp.retroarch\":{"
            "\"game_sort\":\"A-Z\",\"playlist_sort\":\"SYSTEM\"}}}");
}

QString persisted(const QString &dir, const QString &key) {
    QFile f(dir + QStringLiteral("/config.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    return root.value(QStringLiteral("modules")).toObject()
              .value(QLatin1String(kRa)).toObject()
              .value(key).toString();
}
} // namespace

class ProbeBackend : public QObject {
    Q_OBJECT
public:
    int calls = 0;
    QString last;
public slots:
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value) {
        ++calls;
        last = moduleId + QStringLiteral("/") + key + QStringLiteral("=") + value.toString();
    }
};

class AppCoreSettingOrderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void testBackendUpdatedBeforeSignal() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeConfig(dir.path());
        RetroArchBackend backend(QString(), dir.path(), nullptr);
        AppCore core(QString(), dir.path());
        core.registerModule(QLatin1String(kRa), QString(), &backend, nullptr);

        QString seenAtSignal;
        connect(&core, &AppCore::moduleSettingChanged, &core,
                [&](const QString &, const QString &key, const QVariant &) {
                    if (key == QLatin1String("game_sort"))
                        seenAtSignal = backend.gameSort(); // what a QML handler reads
                }, Qt::DirectConnection);

        core.save_setting(QLatin1String(kRa), QStringLiteral("game_sort"),
                          QStringLiteral("Z-A"));

        QCOMPARE(seenAtSignal, QStringLiteral("Z-A"));   // signal saw new state
        QCOMPARE(backend.gameSort(), QStringLiteral("Z-A"));
        QCOMPARE(persisted(dir.path(), QStringLiteral("game_sort")), QStringLiteral("Z-A"));
    }

    void testPlaylistSortIsInvokableAndOrdered() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeConfig(dir.path());
        RetroArchBackend backend(QString(), dir.path(), nullptr);
        AppCore core(QString(), dir.path());
        core.registerModule(QLatin1String(kRa), QString(), &backend, nullptr);

        // Fails at runtime in QML without Q_INVOKABLE (Playlists.qml:322).
        QString viaMeta;
        QVERIFY(QMetaObject::invokeMethod(&backend, "playlistSort", Qt::DirectConnection,
                                          Q_RETURN_ARG(QString, viaMeta)));
        QCOMPARE(viaMeta, QStringLiteral("SYSTEM"));

        QString seenAtSignal;
        connect(&core, &AppCore::moduleSettingChanged, &core,
                [&](const QString &, const QString &key, const QVariant &) {
                    if (key == QLatin1String("playlist_sort"))
                        seenAtSignal = backend.playlistSort();
                }, Qt::DirectConnection);

        core.save_setting(QLatin1String(kRa), QStringLiteral("playlist_sort"),
                          QStringLiteral("COMPANY"));

        QCOMPARE(seenAtSignal, QStringLiteral("COMPANY"));
        QCOMPARE(backend.playlistSort(), QStringLiteral("COMPANY"));
        QCOMPARE(persisted(dir.path(), QStringLiteral("playlist_sort")),
                 QStringLiteral("COMPANY"));
    }

    void testSlotIsNotHandledTwice() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeConfig(dir.path());
        RetroArchBackend backend(QString(), dir.path(), nullptr);
        AppCore core(QString(), dir.path());
        core.registerModule(QLatin1String(kRa), QString(), &backend, nullptr);

        int pathChanges = 0;
        connect(&backend, &RetroArchBackend::retroarchPathChanged, &backend,
                [&]() { ++pathChanges; }, Qt::DirectConnection);
        core.save_setting(QLatin1String(kRa), QStringLiteral("retroarch_path"),
                          QStringLiteral("/opt/retroarch/bin/retroarch"));
        // One push from save_setting; the removed registerModule connection
        // used to fire the same slot a second time.
        QCOMPARE(pathChanges, 1);
    }

    void testOtherModulesStillReceiveChanges() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppCore core(QString(), dir.path());
        ProbeBackend probe; // stands in for VirtualChannels' local_files interest
        core.registerModule(QStringLiteral("com.240mp.local_files"), QString(),
                            &probe, nullptr);

        core.save_setting(QStringLiteral("com.240mp.local_files"),
                          QStringLiteral("media_directory"), QStringLiteral("/srv/media"));
        QCOMPARE(probe.calls, 1);
        QCOMPARE(probe.last,
                 QStringLiteral("com.240mp.local_files/media_directory=/srv/media"));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppCoreSettingOrderTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "AppCoreSettingOrderTest.moc"
