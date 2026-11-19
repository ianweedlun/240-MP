// Regression for T1 M-3 (torture snapshot 3970174): AppCore::registerModule()
// connects the backend's authStateChanged / dynamicOptionsReady / actionStatus
// signals by introspection with neither Qt::UniqueConnection nor a guard
// against re-registering the same backend/module id. Every registration adds
// another connection, so one backend trigger fans out to N AppCore emissions
// after N registrations (lab harness observed 10/10/10 for x10).
//
// Contract pinned here: registering the same backend/module id any number of
// times must still produce exactly ONE AppCore emission per backend trigger,
// and a single registration must keep its 1:1 behaviour and payloads.
//
// The stress arm FAILS BY DESIGN on the unfixed snapshot; the idempotent
// registerModule() fix must turn it green. Do not weaken the stress arm to
// make the suite pass.
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
#include <QVariantList>

#include "AppCore.h"

// Per-signal, named assertion: a failure says which signal multiplied and by
// how much (observed count vs the required single emission).
#define CHECK_ONE_EMISSION(spy, signalName)                                      \
    do {                                                                         \
        const int observed = static_cast<int>((spy).count());                    \
        QVERIFY2(observed == 1,                                                  \
                 qPrintable(QStringLiteral(signalName                            \
                                           ": observed %1 emissions after one "  \
                                           "backend trigger, expected exactly 1") \
                                .arg(observed)));                                \
    } while (false)

namespace {
constexpr auto kModuleId = "com.240mp.probe";
constexpr auto kOptionsKey = "probe_key";
constexpr auto kActionMessage = "probe-done";

// Same probe shape as the T1 lab harness (torture-t1/t1_core.cpp): a bare
// backend exposing exactly the three signals registerModule() introspects.
class ProbeBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void fireAuth() { emit authStateChanged(); }
    void fireOptions()
    {
        emit dynamicOptionsReady(QLatin1String(kOptionsKey), QVariantList{1, 2});
    }
    void fireAction() { emit actionStatus(QLatin1String(kActionMessage)); }
signals:
    void authStateChanged();
    void dynamicOptionsReady(const QString &key, const QVariant &options);
    void actionStatus(const QString &message);
};
} // namespace

class AppCoreReregistrationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void controlOneRegistrationEmitsOnce() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppCore core(QString(), dir.path());
        ProbeBackend backend;
        const QString moduleId = QLatin1String(kModuleId);

        QSignalSpy authSpy(&core, &AppCore::moduleAuthStateChanged);
        QSignalSpy optionsSpy(&core, &AppCore::dynamicOptionsReady);
        QSignalSpy actionSpy(&core, &AppCore::moduleActionStatus);
        QVERIFY(authSpy.isValid());
        QVERIFY(optionsSpy.isValid());
        QVERIFY(actionSpy.isValid());

        core.registerModule(moduleId, QString(), &backend, nullptr);
        backend.fireAuth();
        backend.fireOptions();
        backend.fireAction();

        qInfo("M-3 control (x1 registration): moduleAuthStateChanged=%d "
              "dynamicOptionsReady=%d moduleActionStatus=%d (expected 1/1/1)",
              static_cast<int>(authSpy.count()), static_cast<int>(optionsSpy.count()),
              static_cast<int>(actionSpy.count()));
        CHECK_ONE_EMISSION(authSpy, "moduleAuthStateChanged");
        CHECK_ONE_EMISSION(optionsSpy, "dynamicOptionsReady");
        CHECK_ONE_EMISSION(actionSpy, "moduleActionStatus");

        // Re-emission payloads must carry the module id and pass the
        // backend's arguments through unchanged.
        QCOMPARE(authSpy.first().at(0).toString(), moduleId);
        const QList<QVariant> options = optionsSpy.first();
        QCOMPARE(options.at(0).toString(), moduleId);
        QCOMPARE(options.at(1).toString(), QString(QLatin1String(kOptionsKey)));
        QCOMPARE(options.at(2), QVariant(QVariantList{1, 2}));
        const QList<QVariant> action = actionSpy.first();
        QCOMPARE(action.at(0).toString(), moduleId);
        QCOMPARE(action.at(1).toString(), QString(QLatin1String(kActionMessage)));
    }

    void stressTenRegistrationsEmitOnce() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppCore core(QString(), dir.path());
        ProbeBackend backend;
        const QString moduleId = QLatin1String(kModuleId);

        QSignalSpy authSpy(&core, &AppCore::moduleAuthStateChanged);
        QSignalSpy optionsSpy(&core, &AppCore::dynamicOptionsReady);
        QSignalSpy actionSpy(&core, &AppCore::moduleActionStatus);
        QVERIFY(authSpy.isValid());
        QVERIFY(optionsSpy.isValid());
        QVERIFY(actionSpy.isValid());

        // Same backend, same module id, ten registrations — the M-3 shape.
        for (int i = 0; i < 10; ++i)
            core.registerModule(moduleId, QString(), &backend, nullptr);
        backend.fireAuth();
        backend.fireOptions();
        backend.fireAction();

        qInfo("M-3 stress (x10 registrations): moduleAuthStateChanged=%d "
              "dynamicOptionsReady=%d moduleActionStatus=%d (expected 1/1/1)",
              static_cast<int>(authSpy.count()), static_cast<int>(optionsSpy.count()),
              static_cast<int>(actionSpy.count()));
        CHECK_ONE_EMISSION(authSpy, "moduleAuthStateChanged");
        CHECK_ONE_EMISSION(optionsSpy, "dynamicOptionsReady");
        CHECK_ONE_EMISSION(actionSpy, "moduleActionStatus");
    }
};

#undef CHECK_ONE_EMISSION

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppCoreReregistrationTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "AppCoreReregistrationTest.moc"
