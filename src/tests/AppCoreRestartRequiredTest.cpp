// Unit tests for AppCore::restart_required() and its pure helper
// moduleStateChangedSinceBoot().
//
// Compares the on-disk module enable flags (config.json) against the
// backends loaded this session (m_backends). A mismatch means a module
// change takes effect only on restart, so the Settings banner shows.
//
// Cases:
//   no change                        -> false
//   module enabled, no backend       -> true
//   module disabled, backend live    -> true
//   VC dependency built, nullptr     -> false (disabled config)
//
// No QML engine, no network, no backend, no DisplayHandoff.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QString>

#include "AppCore.h"

namespace {

constexpr auto kPlex = "com.240mp.plex";

void writeConfig(const QString &dir, const QJsonObject &modules) {
    QFile f(dir + "/config.json");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonObject root;
    root["modules"] = modules;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void writeManifest(const QString &dir) {
    QDir mDir(dir + "/modules/com.240mp.plex");
    QVERIFY(mDir.mkpath("."));
    QFile f(mDir.absoluteFilePath("manifest.json"));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonObject manifest;
    manifest["id"] = "com.240mp.plex";
    manifest["name"] = "Plex";
    manifest["folder"] = "plex";
    manifest["entry_point_qml"] = "views/Root.qml";
    manifest["icon"] = "icon.svg";
    manifest["settings"] = QJsonArray();
    f.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
}

class DummyBackend : public QObject {
    Q_OBJECT
public:
    explicit DummyBackend(QObject *parent = nullptr) : QObject(parent) {}
};

class AppCoreRestartRequiredTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // Config matches loaded backends: nothing to restart.
    void testNoChangeReturnsFalse() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonObject modules;
        modules[kPlex] = QJsonObject{{"enabled", false}};
        writeConfig(dir.path(), modules);
        writeManifest(dir.path());

        AppCore core(dir.path(), dir.path());
        // enabled=false, hasBackend=false -> false.
        QVERIFY(!core.restart_required());
    }

    // Module enabled in config but no backend loaded this session.
    void testEnabledNoBackendReturnsTrue() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonObject modules;
        modules[kPlex] = QJsonObject{{"enabled", true}};
        writeConfig(dir.path(), modules);
        writeManifest(dir.path());

        AppCore core(dir.path(), dir.path());
        // enabled=true, hasBackend=false -> true.
        QVERIFY(core.restart_required());
    }

    // Module disabled in config, but a backend is still registered.
    void testDisabledBackendLoadedReturnsTrue() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonObject modules;
        modules[kPlex] = QJsonObject{{"enabled", false}};
        writeConfig(dir.path(), modules);
        writeManifest(dir.path());

        AppCore core(dir.path(), dir.path());
        DummyBackend backend;
        core.registerModule(kPlex, "dummyBackend", &backend, nullptr);
        // enabled=false, hasBackend=true -> true.
        QVERIFY(core.restart_required());
    }

    // VC dependency (plex) is built but registered as nullptr while config
    // disables it: hasBackend=false, enabled=false -> no restart required.
    void testVCDependencyNullRegisteredWithDisabledConfigReturnsFalse() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonObject modules;
        modules[kPlex] = QJsonObject{{"enabled", false}};
        writeConfig(dir.path(), modules);
        writeManifest(dir.path());

        AppCore core(dir.path(), dir.path());
        // Built but not registered (null backend = unregister).
        core.registerModule(kPlex, "plexBackend", nullptr, nullptr);
        // enabled=false, hasBackend=false -> false.
        QVERIFY(!core.restart_required());
    }
};

}  // close anonymous namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppCoreRestartRequiredTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "AppCoreRestartRequiredTest.moc"
