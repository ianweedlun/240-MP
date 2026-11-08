#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>

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

// WCAG 2.x relative-luminance contrast ratio for two #RRGGBB strings. The
// contrast contract for the theme table: every text/fill role pair the views
// actually render must clear the AA floor against the fill it draws on. The
// pair list below was walked mechanically over the view sources (fill-bearing
// items with Text descendants; conditional branches paired by identical
// conditions) on the dev tip 2026-11-08 — see the audit script at
// /home/ian/src/vibes/theme_audit.py for the same table.
constexpr double kAaFloor = 4.5;

double srgbChannel(int c)
{
    const double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QString &hex)
{
    const QString h = hex.startsWith('#') ? hex.mid(1) : hex;
    if (h.size() < 6)
        return -1.0;
    auto hexVal = [](QChar c) {
        if (c >= '0' && c <= '9')
            return c.digitValue();
        const QChar l = c.toLower();
        if (l >= 'a' && l <= 'f')
            return 10 + (l.unicode() - u'a');
        return -1;
    };
    auto byteAt = [&](int i) {
        const int hi = hexVal(h.at(i));
        const int lo = hexVal(h.at(i + 1));
        return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
    };
    const int r = byteAt(0), g = byteAt(2), b = byteAt(4);
    if (r < 0 || g < 0 || b < 0)
        return -1.0;
    return 0.2126 * srgbChannel(r) + 0.7152 * srgbChannel(g) + 0.0722 * srgbChannel(b);
}

double contrastRatio(const QString &a, const QString &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    if (la < 0.0 || lb < 0.0)
        return -1.0;
    const double hi = qMax(la, lb);
    const double lo = qMin(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}
}

class ColorSchemesTest : public QObject {
    Q_OBJECT

private slots:
    void builtInThemesHaveFiveWellFormedKeys();
    void pickerOffersExactlyBuiltInThemes();
    void newFeatureSchemesArePresent();
    void renderedRolePairsClearAaFloor();
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

// The five (text-role, fill-role) pairs the app actually renders, taken
// verbatim from the mechanical walk over all module + core views: every
// fill-bearing item with Text descendants, branches paired by identical
// conditions, translucent fills skipped. If a view starts painting text on a
// new role combination, this list grows with it and the contract re-checks all
// palettes against it.
void ColorSchemesTest::renderedRolePairsClearAaFloor()
{
    const QString root = findRepoRoot();
    QVERIFY(!root.isEmpty());
    const QMap<QString, QString> themes = readThemes(root);
    QVERIFY(!themes.isEmpty());

    const QStringList pairs{
        "tertiary surface",  // list headers + rules on the page field
        "primary surface",   // row titles on the page field
        "secondary surface", // secondary/preview text on the page field
        "surface accent",    // selected-row labels on the accent bar
        "accent surface",    // accent-painted labels on the page field
    };

    QRegularExpression hexRe(
        QStringLiteral("\"(primary|secondary|tertiary|surface|accent)\"\\s*:\\s*\"(#[0-9A-Fa-f]{6})\""));
    for (auto it = themes.begin(); it != themes.end(); ++it) {
        QHash<QString, QString> colors;
        QRegularExpressionMatchIterator mi = hexRe.globalMatch(it.value());
        while (mi.hasNext()) {
            const QRegularExpressionMatch m = mi.next();
            const QStringList caps = m.capturedTexts();
            if (!colors.contains(caps[1]))
                colors.insert(caps[1], caps[2]);
        }
        for (const QString &pair : pairs) {
            const QStringList roles = pair.split(' ');
            if (!colors.contains(roles[0]) || !colors.contains(roles[1]))
                continue; // malformed entries are caught by the key tests
            const QString ta = colors[roles[0]], fb = colors[roles[1]];
            const double got = contrastRatio(ta, fb);
            QVERIFY2(got >= kAaFloor,
                     qPrintable(QStringLiteral("theme=%1 pair=%2-on-%3 text=%4 fill=%5 ratio=%6")
                                    .arg(it.key()).arg(roles[0]).arg(roles[1])
                                    .arg(ta).arg(fb).arg(got, 0, 'f', 2)));
        }
    }
}

QTEST_MAIN(ColorSchemesTest)
#include "ColorSchemesTest.moc"
