// See MpvBasicOptions.h. The menu and ~/.config/mpv/mpv.conf are the same
// thing now: table-driven parse + write-back, no override layer. "#" lines
// are comments, the LAST active assignment wins (matching mpv), and one
// `include <file>` level is followed so split configs display correctly.
// Everything except the touched line is byte-preserved on write.
#include "MpvBasicOptions.h"
#include <initializer_list>
#include <QByteArray>
#include <QFileInfo>
#include <QDir>
#include <QChar>

namespace mpvopts {
namespace {

static QString confDir(const QString &path)
{
    const int sep = path.lastIndexOf('/');
    return sep > 0 ? path.left(sep) : QStringLiteral(".");
}

struct ChoiceRow {
    const char *key;        // managed settings key
    const char *option;     // mpv option name
    const char *defaultRaw; // mpv's built-in default
    // menu token -> mpv value.
    std::initializer_list<std::pair<const char *, const char *>> choices;
};

struct NumericRow {
    const char *key;
    const char *option;
    int lo;
    int hi;
    std::initializer_list<const char *> steps; // menu tokens == mpv values
    const char *defaultRaw;
};

const ChoiceRow choiceRows[] = {
    {"replaygain", "replaygain", "auto", {
        {"Off", "no"}, {"Track", "track"}, {"Album", "album"}}},
    {"deinterlace", "deinterlace", "no", {
        {"Off", "no"}, {"Auto", "auto"}, {"On", "yes"}}},
    {"audio_channels", "audio-channels", "auto", {
        {"Auto", "max"}, {"Stereo", "stereo"}, {"Mono", "1"}}},
};
const NumericRow numericRows[] = {
    {"audio_gain", "audio-gain", -20, 20,
        {"-4", "0", "2", "4", "6", "8"}, "0"},
};

inline QString opt(const char *s) { return QString::fromUtf8(s); }

// Underscore spellings in a hand-edited conf match the hyphen option name.
QString normalizedOption(const QString &rawKey)
{
    QString k = rawKey.trimmed().toLower();
    k.replace(('_'), QStringLiteral("-"));
    return k;
}

bool isPlainNumber(const QString &s)
{
    if (s.isEmpty())
        return false;
    int i = s.startsWith(('-')) ? 1 : 0;
    if (i >= s.size())
        return false;
    bool digits = false;
    bool seenDot = false;
    for (; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c.isDigit()) {
            digits = true;
        } else if (c == ('.') && !seenDot) {
            seenDot = true;
        } else {
            return false;
        }
    }
    return digits;
}

// Conf -> option -> raw value of the last active assignment. One include
// level: main file parses with includeDepth=1; included files get 0.
void parseConf(const QString &path, QHash<QString, QString> &values, int includeDepth)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return; // missing file: defaults apply
    const QStringList lineList = QString::fromUtf8(f.readAll()).split(('\n'));
    for (const QString &rawLine : lineList) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QChar('#')))
            continue;
        if (line.startsWith(QStringLiteral("include"))) {
            if (includeDepth <= 0)
                continue;
            QString target = line.mid(7).trimmed();
            if (target.length() >= 2 && target.startsWith(QChar('<')) && target.endsWith(QChar('>')))
                target = target.mid(1, target.length() - 2).trimmed();
            if (target.isEmpty() || target.startsWith(QChar('~')))
                continue; // tilde expansion out of scope here
            if (!target.startsWith(QChar('/')))
                target = confDir(path) + QStringLiteral("/") + target;
            parseConf(target, values, includeDepth - 1);
            continue;
        }
        const int eq = line.indexOf(QChar('='));
        if (eq < 0)
            continue;
        const QString key = normalizedOption(line.left(eq));
        const QString value = line.mid(eq + 1).trimmed();
        for (const ChoiceRow &row : choiceRows)
            if (key == opt(row.option))
                values.insert(opt(row.option), value);
        for (const NumericRow &row : numericRows)
            if (key == opt(row.option))
                values.insert(opt(row.option), value);
    }
}

QHash<QString, QString> parsedOptions(const QString &confPath)
{
    QHash<QString, QString> values;
    parseConf(confPath, values, 1);
    return values;
}

// Display token for a raw value: menu token when known, honest
// "Custom (raw)" otherwise (also for built-in defaults the menu omits).
QString displayTokenFor(const QString &key, const QString &raw)
{
    for (const ChoiceRow &row : choiceRows) {
        if (key != opt(row.key))
            continue;
        for (const auto &pair : row.choices)
            if (raw == opt(pair.second))
                return opt(pair.first);
        return QStringLiteral("Custom (%1)").arg(raw);
    }
    for (const NumericRow &row : numericRows) {
        if (key != opt(row.key))
            continue;
        for (const char *step : row.steps)
            if (raw == opt(step))
                return opt(step);
        // "0.0" and "0" are the same menu step; anything else stays honest.
        if (isPlainNumber(raw)) {
            const double want = raw.toDouble();
            for (const char *step : row.steps) {
                const double v = opt(step).toDouble();
                const double diff = want - v;
                if (diff > -1e-9 && diff < 1e-9)
                    return opt(step);
            }
        }
        return QStringLiteral("Custom (%1)").arg(raw);
    }
    return raw;
}

// menu token -> mpv value. Unknown token -> false (file untouched).
bool tokenToMpvValue(const QString &key, const QString &token, QString &outValue)
{
    for (const ChoiceRow &row : choiceRows) {
        if (key != opt(row.key))
            continue;
        for (const auto &pair : row.choices)
            if (token == opt(pair.first)) {
                outValue = opt(pair.second);
                return true;
            }
        return false;
    }
    for (const NumericRow &row : numericRows) {
        if (key != opt(row.key))
            continue;
        for (const char *step : row.steps)
            if (token == opt(step)) {
                outValue = opt(step);
                return true;
            }
        return false;
    }
    return false;
}

QString optionForKey(const QString &key)
{
    for (const ChoiceRow &row : choiceRows)
        if (key == opt(row.key))
            return opt(row.option);
    for (const NumericRow &row : numericRows)
        if (key == opt(row.key))
            return opt(row.option);
    return {};
}

} // anonymous namespace

QVariantMap effectiveTokens(const QString &mpvConfPath)
{
    const QHash<QString, QString> found = parsedOptions(mpvConfPath);
    QVariantMap out;
    for (const ChoiceRow &row : choiceRows) {
        QString raw = found.value(opt(row.option));
        if (raw.isEmpty())
            raw = opt(row.defaultRaw);
        out.insert(opt(row.key), displayTokenFor(opt(row.key), raw));
    }
    for (const NumericRow &row : numericRows) {
        QString raw = found.value(opt(row.option));
        if (raw.isEmpty())
            raw = opt(row.defaultRaw);
        out.insert(opt(row.key), displayTokenFor(opt(row.key), raw));
    }
    return out;
}

bool setUiValue(const QString &mpvConfPath, const QString &key, const QString &uiToken)
{
    if (uiToken == QStringLiteral("Mpv Conf")) // legacy save: no-op
        return true;
    QString mpvValue;
    if (!tokenToMpvValue(key, uiToken, mpvValue))
        return false;
    const QString option = optionForKey(key);
    if (option.isEmpty())
        return false;

    QFile f(mpvConfPath);
    QByteArray content;
    if (f.open(QIODevice::ReadOnly)) {
        content = f.readAll();
        f.close();
    }
    const QStringList lines = content.isEmpty()
                                 ? QStringList()
                                 : QString::fromUtf8(content).split(('\n'));
    int target = -1;
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(('#')))
            continue;
        const int eq = line.indexOf(('='));
        if (eq < 0)
            continue;
        if (normalizedOption(line.left(eq)) == option) {
            target = i;
            break;
        }
    }
    const QString newLine = option + ('=') + mpvValue;
    if (target >= 0) {
        QStringList edited = lines;
        edited[target] = newLine;
        content = edited.join(('\n')).toUtf8();
    } else {
        if (!content.isEmpty() && !content.endsWith('\n'))
            content += QByteArrayLiteral("\n");
        content += (newLine + QStringLiteral("\n")).toUtf8();
    }
    QDir().mkpath(confDir(mpvConfPath));
    QFile out(mpvConfPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    out.write(content);
    return true;
}

QString mpvValueForLegacyToken(const QString &key, const QString &legacyToken)
{
    if (legacyToken == QStringLiteral("Mpv Conf"))
        return QString();
    QString v;
    tokenToMpvValue(key, legacyToken, v);
    return v;
}

bool isManagedKey(const QString &key)
{
    return !optionForKey(key).isEmpty();
}

} // namespace mpvopts
