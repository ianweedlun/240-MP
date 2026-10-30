// See MpvBasicOptions.h. Table-driven: one row per knob, validation inside,
// never a passthrough for a value we don't trust. Read-at-call-site matches
// mpv_video_args so a config edit applies to the next spawn without a rebuild.
#include "MpvBasicOptions.h"
#include <initializer_list>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

namespace {

struct ChoiceRow {
    const char *key;   // settings key in config.json "app"
    const char *option; // mpv CLI option name
    // UI token -> mpv value. The "Mpv Conf" token maps to empty (= emit
    // nothing, follow the global config file), same as an absent key.
    std::initializer_list<std::pair<const char *, const char *>> choices;
};

struct NumericRow {
    const char *key;
    const char *option;
    int lo;
    int hi;
};

const ChoiceRow choiceRows[] = {
    {"replaygain", "replaygain", {
        {"Mpv Conf", ""}, {"Off", "no"}, {"Track", "track"}, {"Album", "album"}}},
    {"deinterlace", "deinterlace", {
        {"Mpv Conf", ""}, {"Off", "no"}, {"Auto", "auto"}, {"On", "yes"}}},
    {"audio_channels", "audio-channels", {
        {"Mpv Conf", ""}, {"Auto", "max"}, {"Stereo", "stereo"}, {"Mono", "1"}}},
};

const NumericRow numericRows[] = {
    {"audio_gain", "audio-gain", -20, 20}, // dB; bounded so a typo can't flatten everything
};

QJsonObject loadAppSection(const QString &dataRoot)
{
    QFile f(dataRoot + "/config.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object()["app"].toObject();
}

} // namespace

namespace mpvopts {

QStringList basicArgs(const QString &dataRoot)
{
    const QJsonObject app = loadAppSection(dataRoot);
    if (app.isEmpty())
        return {};

    QStringList args;
    for (const ChoiceRow &row : choiceRows) {
        const QString v = app.value(row.key).toString().trimmed();
        if (v.isEmpty())
            continue;
        bool emitted = false;
        for (const auto &pair : row.choices) {
            if (v == QLatin1String(pair.first)) {
                if (pair.second[0] != '\0')
                    args << QStringLiteral("--%1=%2").arg(row.option).arg(QLatin1String(pair.second));
                emitted = true;
                break;
            }
        }
        // A token we don't recognize (typo, stale config) is skipped — never
        // forwarded to mpv.
        Q_UNUSED(emitted);
    }
    for (const NumericRow &row : numericRows) {
        const QString v = app.value(row.key).toString().trimmed();
        if (v.isEmpty())
            continue;
        bool ok = false;
        const int n = v.toInt(&ok);
        if (ok && n >= row.lo && n <= row.hi)
            args << QStringLiteral("--%1=%2").arg(row.option).arg(n);
    }
    return args;
}

} // namespace mpvopts
