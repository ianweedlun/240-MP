#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>

class WriteFileProbeTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    void doWriteRead(const QString &label, const QString &content) {
        const QString path = m_dir.filePath("weather_location.txt");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const qint64 n = f.write(content.toUtf8());
        f.close();
        QFile r(path);
        QVERIFY(r.open(QIODevice::ReadOnly));
        qInfo("[probe] %s: requested=%lld written=%lld readback=%lld exists=%d",
              qUtf8Printable(label), (long long)content.toUtf8().size(),
              (long long)n, (long long)r.size(), QFileInfo(path).exists());
    }
public slots:
    void testSingleLine() { doWriteRead("singleLine", "38.8, -77.0, OLD\n"); }
    void testMultiLine() { doWriteRead("multiLine", "# documented list\n\n42.3601, -71.0589, BOSTON\n# extra note\n40.7128, -74.0060, NYC\n"); }
};

#include "WriteFileProbeTest.moc"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    WriteFileProbeTest testobject;
    return QTest::qExec(&testobject, argc, argv);
}
