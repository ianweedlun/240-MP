// See MpvBasicOptions.h. The menu and ~/.config/mpv/mpv.conf are the same
// thing now: table-driven parse + write-back, no override layer. "#" lines
// are comments, the LAST active assignment wins (matching mpv), and one
// `include <file>` level is followed so split configs display correctly.
// Everything except the touched line is byte-preserved on write.
#include "MpvBasicOptions.h"
#include <initializer_list>
#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QChar>
// POSIX includes come AFTER Qt: qsavefile.h refuses to be included after a
// header that defines open(2).
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace mpvopts {
namespace {

static QString confDir(const QString &path)
{
    const int sep = path.lastIndexOf('/');
    return sep > 0 ? path.left(sep) : QStringLiteral(".");
}

// ---- CFG-01 N4b: atomic mpv.conf persistence -----------------------------
// Mirrors cfgfile::atomicWrite (the shared CFG-01 shape, fix-decision D1):
// QSaveFile with EVERY return checked, file fsync before commit and containing
// directory fsync after commit. Any pre-commit failure leaves the live file
// byte-identical and discards the temp; a directory-fsync failure returns
// false with the NEW COMPLETE file already in place (never a partial). The
// fault seam is compiled ONLY into the two-sided gate target, never the app.
static bool mpvconfSeamFail(const char *stage)
{
#ifdef MPVCONF_TEST_SEAM
    const QByteArray want = qgetenv("MPVCONF_SEAM_FAIL");
    if (!want.isEmpty() && want == stage) {
        qWarning("[mpvopts] seam %s injected", stage);
        return true;
    }
#else
    Q_UNUSED(stage);
#endif
    return false;
}

static bool mpvconfFsyncDir(const QString &path)
{
    const QString dir = QFileInfo(path).absolutePath();
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(QFile::encodeName(dir).constData(), flags);
    if (fd < 0) {
        qWarning("[mpvopts] dir-fsync open failed for %s: %s",
                 qPrintable(dir), strerror(errno));
        return false;
    }
    const int result = ::fsync(fd);
    const int savedErrno = errno;
    ::close(fd);
    if (result != 0) {
        qWarning("[mpvopts] dir-fsync failed for %s: %s",
                 qPrintable(dir), strerror(savedErrno));
        return false;
    }
    return true;
}

static bool atomicWriteText(const QString &path, const QByteArray &content)
{
    if (mpvconfSeamFail("open"))
        return false;
    QSaveFile out(path);
    out.setDirectWriteFallback(false); // never truncate the live file
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("[mpvopts] atomic write open failed for %s: %s",
                 qPrintable(path), qPrintable(out.errorString()));
        return false;
    }
    const qint64 written = out.write(content.constData(), content.size());
    const bool seamWrite = mpvconfSeamFail("write");
    if (written != content.size() || seamWrite) {
        qWarning("[mpvopts] atomic write failed for %s (%lld of %lld bytes): %s",
                 qPrintable(path), static_cast<long long>(written),
                 static_cast<long long>(content.size()), qPrintable(out.errorString()));
        out.cancelWriting();
        return false;
    }
    const bool seamFlush = mpvconfSeamFail("flush");
    if (!out.flush() || seamFlush) {
        qWarning("[mpvopts] flush failed for %s: %s",
                 qPrintable(path), qPrintable(out.errorString()));
        out.cancelWriting();
        return false;
    }
    const int fd = static_cast<int>(out.handle());
    const bool seamFsync = mpvconfSeamFail("fsync");
    if (seamFsync || fd < 0 || ::fsync(fd) != 0) {
        qWarning("[mpvopts] file fsync failed for %s: %s", qPrintable(path),
                 seamFsync ? "injected" : strerror(errno));
        out.cancelWriting();
        return false;
    }
    if (mpvconfSeamFail("commit")) {
        qWarning("[mpvopts] commit failure injected for %s", qPrintable(path));
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        qWarning("[mpvopts] commit failed for %s: %s",
                 qPrintable(path), qPrintable(out.errorString()));
        return false;
    }
    if (mpvconfSeamFail("dirfsync") || !mpvconfFsyncDir(path)) {
        qWarning("[mpvopts] dir fsync failed for %s (live file is the new complete document)",
                 qPrintable(path));
        return false;
    }
    return true;
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

    QByteArray content;
    if (QFile::exists(mpvConfPath)) {
        // An existing file that cannot be read must NOT be treated as empty:
        // that would rewrite the whole conf as a single line and silently
        // destroy every other mpv setting (the text-format analogue of
        // CFG-01's defaults-over-corrupt wipe). Refuse and leave it untouched.
        const bool seamRead = mpvconfSeamFail("readopen");
        QFile f(mpvConfPath);
        if (seamRead || !f.open(QIODevice::ReadOnly)) {
            qWarning("[mpvopts] cannot read existing mpv.conf %s; refusing to write: %s",
                     qPrintable(mpvConfPath),
                     seamRead ? "injected" : qPrintable(f.errorString()));
            return false;
        }
        content = f.readAll();
        if (f.error() != QFileDevice::NoError) {
            qWarning("[mpvopts] read error on %s; refusing to write: %s",
                     qPrintable(mpvConfPath), qPrintable(f.errorString()));
            return false;
        }
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
    if (!atomicWriteText(mpvConfPath, content)) {
        qWarning("[mpvopts] could not persist mpv.conf %s; live file left intact",
                 qPrintable(mpvConfPath));
        return false;
    }
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
