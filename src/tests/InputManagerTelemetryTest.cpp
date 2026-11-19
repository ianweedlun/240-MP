// Key-decision telemetry must observe the raw keyboard/remote channel too.
// The Pi control server injects ordinary Qt key events; those keys navigate
// QML directly and never pass through deliverPress(). These tests use the real
// AppCore state log and the real InputManager event filter, offscreen.
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include "AppCore.h"
#include "input/InputManager.h"

namespace {
constexpr auto kStateLog = "/tmp/240mp_state.log";
constexpr auto kKeyInput = "/tmp/240mp_key.in";

class TestInputManager : public InputManager {
public:
    using InputManager::InputManager;
    bool filter(QKeyEvent *event) { return eventFilter(nullptr, event); }
};

struct Fixture {
    QTemporaryDir dir;
    AppCore core;
    TestInputManager input;

    Fixture()
        : core(QString::fromUtf8(MP240_REPO_ROOT), dir.path())
        , input(dir.path(), &core)
    {}
};

QStringList keyLines()
{
    QFile file(QString::fromLatin1(kStateLog));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList result;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(
        QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.contains(QLatin1String(" key ")))
            result << line;
    }
    return result;
}

class InputManagerTelemetryTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        QFile::remove(QString::fromLatin1(kStateLog));
        QFile::remove(QString::fromLatin1(kKeyInput));
    }

    void rawKeyTapsEmitDeliveredLines()
    {
        Fixture fixture;
        for (int tap = 0; tap < 2; ++tap) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QVERIFY(!fixture.input.filter(&press));
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
            QVERIFY(!fixture.input.filter(&release));
        }
        const QStringList lines = keyLines();
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].endsWith(QLatin1String("key down delivered")));
        QVERIFY(lines[1].endsWith(QLatin1String("key down delivered")));
    }

    void rawAutoRepeatDoesNotSpamTelemetry()
    {
        Fixture fixture;
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QVERIFY(!fixture.input.filter(&press));
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier, QString(), true);
        QVERIFY(!fixture.input.filter(&repeat));
        QCOMPARE(keyLines().size(), 1);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
        QVERIFY(!fixture.input.filter(&release));
        QKeyEvent freshPress(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QVERIFY(!fixture.input.filter(&freshPress));
        QCOMPARE(keyLines().size(), 2);
    }

    void debugKeyChannelLogsEveryTap()
    {
        Fixture fixture;
        QQuickWindow window;
        fixture.input.setTargetWindow(&window);
        window.show();
        window.requestActivate();
        QTRY_VERIFY(window.isActive());

        for (int tap = 1; tap <= 2; ++tap) {
            QFile input(QString::fromLatin1(kKeyInput));
            QVERIFY(input.open(QIODevice::Append | QIODevice::Text));
            QCOMPARE(input.write("down\n"), qint64(5));
            input.close();
            QTRY_COMPARE(keyLines().size(), tap);
            QVERIFY(keyLines().constLast().endsWith(QLatin1String("key down delivered")));
        }
    }

    void remappedKeyReleaseRearmsTelemetry()
    {
        Fixture fixture;
        fixture.core.save_setting(QString(), QStringLiteral("remote_keymap.down"),
                                  int(Qt::Key_F6));
        QQuickWindow window;
        fixture.input.setTargetWindow(&window);
        window.show();
        window.requestActivate();
        QTRY_VERIFY(window.isActive());

        for (int tap = 0; tap < 2; ++tap) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_F6, Qt::NoModifier);
            QVERIFY(fixture.input.filter(&press));
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_F6, Qt::NoModifier);
            QVERIFY(fixture.input.filter(&release));
        }
        const QStringList lines = keyLines();
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].endsWith(QLatin1String("key down delivered")));
        QVERIFY(lines[1].endsWith(QLatin1String("key down delivered")));
    }
};
} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    InputManagerTelemetryTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "InputManagerTelemetryTest.moc"
