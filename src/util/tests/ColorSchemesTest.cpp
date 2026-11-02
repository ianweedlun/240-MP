#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

// Palette-consistency tests for built-in color schemes: pure text-in, checks-out.
// Parses the themes map in Main.qml and the colorOpts list in views/Settings.qml
// and asserts they agree: every built-in theme has exactly the five documented
// keys with well-formed 6-digit hex, and every built-in appears exactly once in
// the Settings picker. No QML engine involved; reads the working-tree sources.
namespace {
QString findRepoRoot()
{
    // Tests run from a build dir inside the repo; walk up until both sources exist.
    QString dir = QDir::currentPath();
    for (int up = 0; up < 6 && !dir.isEmpty(); ++up) {
        if (QFile::exists(dir + "/Main.qml") && QFile::exists(dir + "/views/Settings.qml"))
            return dir;
        const int sep = dir.lastIndexOf('/');
        dir = (sep > 0) ? dir.left(sep) : QString();
    }
    return {};
}

// name -> raw entry body, for every "Name": { ... } entry in the themes map.
QMap<QString, QString> readThemes(const QString &root)
{
    QMap<QString, QString> out;
    QFile f(root + "/Main.qml");
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QString src = f.readAll();
    const int start = src.indexOf("property var themes");
    const int end = src.indexOf("property var allThemes");
    if (start < 0 || end <= start)
        return out;
    const QString seg = src.mid(start, end - start);
    QRegularExpression entryRe(QStringLiteral("\"([^\":\\n]+)\"\\s*:\\s*\\{"));
    QVector<int> bodyEnds;
    QVector<QString> names;
    QRegularExpressionMatchIterator it = entryRe.globalMatch(seg);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        names.append(m.captured(1));
        bodyEnds.append(int(m.capturedEnd(0)));
    }
    for (int i = 0; i < names.size(); ++i) {
        const int bodyStart = bodyEnds[i];
        int bodyStop = seg.size();
        QRegularExpressionMatch m2 = entryRe.match(seg, bodyStart);
        if (m2.isValid())
            bodyStop = int(m2.capturedStart(0));
        out.insert(names[i], seg.mid(bodyStart, bodyStop - bodyStart));
    }
    return out;
}

QStringList readPickerOptions(const QString &root)
{
    QStringList out;
    QFile f(root + "/views/Settings.qml");
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QString src = f.readAll();
    const int decl = src.indexOf("var colorOpts = [");
    if (decl < 0)
        return out;
    const int close = src.indexOf(']', decl);
    if (close < 0)
        return out;
    const QString arr = src.mid(decl, close - decl);
    QRegularExpression strRe(QStringLiteral("\"([^\"]*)\""));
    QRegularExpressionMatchIterator it = strRe.globalMatch(arr);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        out.append(m.captured(1));
    }
    return out;
}
}

class ColorSchemesTest : public QObject {
    Q_OBJECT

private slots:
    void builtInThemesHaveFiveWellFormedKeys();
    void pickerOffersExactlyBuiltInThemes();
    void newFeatureSchemesArePresent();
};

void ColorSchemesTest::builtInThemesHaveFiveWellFormedKeys()
{
    const QString root = findRepoRoot();
    QVERIFY(!root.isEmpty());
    const QMap<QString, QString> themes = readThemes(root);
    QVERIFY(!themes.isEmpty());
    QVERIFY(themes.size() >= 13); // eight pre-existing + five added by this feature
    QRegularExpression hexRe(
        QStringLiteral("\"(primary|secondary|tertiary|surface|accent)\"\\s*:\\s*\"#[0-9A-Fa-f]{6}\""));
    for (auto it = themes.begin(); it != themes.end(); ++it) {
        int n = 0;
        QRegularExpressionMatchIterator mi = hexRe.globalMatch(it.value());
        while (mi.hasNext()) {
            mi.next();
            ++n;
        }
        QCOMPARE(n, 5);
    }
}

void ColorSchemesTest::pickerOffersExactlyBuiltInThemes()
{
    const QString root = findRepoRoot();
    QVERIFY(!root.isEmpty());
    const QMap<QString, QString> themes = readThemes(root);
    const QStringList opts = readPickerOptions(root);
    QVERIFY(!opts.isEmpty());
    QSet<QString> optSet(opts.begin(), opts.end());
    QVERIFY(optSet.size() == opts.size()); // no duplicate picker entries
    QSet<QString> names;
    for (auto it = themes.begin(); it != themes.end(); ++it)
        names.insert(it.key());
    QCOMPARE(optSet == names, true); // built-in names and picker list agree exactly
}

void ColorSchemesTest::newFeatureSchemesArePresent()
{
    const QString root = findRepoRoot();
    QVERIFY(!root.isEmpty());
    const QMap<QString, QString> themes = readThemes(root);
    const QStringList opts = readPickerOptions(root);
    const QStringList added{"Dracula", "Tokyo Night", "Nord", "Solarized Dark", "Monokai"};
    for (const QString &name : added) {
        QVERIFY(themes.contains(name));
        QVERIFY(opts.contains(name));
    }
}

QTEST_MAIN(ColorSchemesTest)
#include "ColorSchemesTest.moc"
