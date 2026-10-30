#pragma once
// Pure WMO 4677 / METAR code-mapping functions for the weather module.
//
// These live in a header (static inline) rather than the anonymous namespace
// of WeatherBackend.cpp so the test suite can exercise them without a display,
// a network or a WeatherBackend instance. WeatherBackend.cpp includes this
// file and keeps only the I/O-heavy code.

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QtMath>

namespace wxcodes {

// Private extension to WMO 4677, which has no code for a broken deck: it offers
// four sky states (0–3) where METAR reports five. Codes at 100+ are ours, are
// never emitted by Open-Meteo, and share the domain so that anything mapping
// from codes — the planned icons, most of all — needs only one lookup table.
constexpr int kCodeMostlyCloudy = 103;

// WMO 4677 weather interpretation codes, as short uppercase strings.
//
// Note these merge two things a METAR keeps separate: sky condition (CLEAR,
// OVERCAST) and present weather (RAIN, FOG, THUNDERSTORM). One code, so one
// condition line — which is what the WeatherStar layout wants anyway.
static inline QString conditionForCode(int code) {
    switch (code) {
    case 0:  return QStringLiteral("CLEAR");
    case 1:  return QStringLiteral("MAINLY CLEAR");
    case 2:  return QStringLiteral("PARTLY CLOUDY");
    case 3:  return QStringLiteral("OVERCAST");
    case 45: case 48:            return QStringLiteral("FOG");
    case 51: case 53: case 55:   return QStringLiteral("DRIZZLE");
    case 56: case 57:            return QStringLiteral("FREEZING DRIZZLE");
    case 61: case 63: case 65:   return QStringLiteral("RAIN");
    case 66: case 67:            return QStringLiteral("FREEZING RAIN");
    case 71: case 73: case 75:   return QStringLiteral("SNOW");
    case 77:                     return QStringLiteral("SNOW GRAINS");
    case 80: case 81: case 82:   return QStringLiteral("SHOWERS");
    case 85: case 86:            return QStringLiteral("SNOW SHOWERS");
    case 79:                     return QStringLiteral("SLEET");
    case 89:                     return QStringLiteral("HAIL");
    case 95:                     return QStringLiteral("THUNDERSTORM");
    case 96: case 99:            return QStringLiteral("THUNDERSTORM HAIL");
    case kCodeMostlyCloudy:      return QStringLiteral("MOSTLY CLOUDY");
    default: break;
    }
    return QStringLiteral("UNKNOWN");
}

// Short forms for the Extended Forecast columns, which are a third of the screen
// wide. Long single words are the problem — WordWrap can't break "THUNDERSTORM",
// so it runs into the next column. The original had the same split: full wording
// on Current Conditions, abbreviations across the three-day columns.
//
// These also read as a *forecast* rather than an observation: a clear day ahead
// is SUNNY, whereas conditions right now are CLEAR. Hence isDay — the Extended
// Forecast columns always pass true, because a forecast for a whole day is a
// daytime characterisation by definition, but Other Locations reuses these
// short forms for *current* conditions in a narrow column and would otherwise
// call a clear sky SUNNY at three in the morning.
static inline QString conditionShortForCode(int code, bool isDay) {
    switch (code) {
    case 0:                      return isDay ? QStringLiteral("SUNNY")
                                              : QStringLiteral("CLEAR");
    case 1:                      return isDay ? QStringLiteral("SUNNY")
                                              : QStringLiteral("MAINLY CLEAR");
    case 2:                      return QStringLiteral("PARTLY CLOUDY");  // wraps to two lines
    case 3:                      return QStringLiteral("CLOUDY");
    case 45: case 48:            return QStringLiteral("FOG");
    case 51: case 53: case 55:   return QStringLiteral("DRIZZLE");
    case 56: case 57:            return QStringLiteral("FRZ DRIZZLE");
    case 61: case 63: case 65:   return QStringLiteral("RAIN");
    case 66: case 67:            return QStringLiteral("FRZ RAIN");
    case 71: case 73: case 75:   return QStringLiteral("SNOW");
    case 77:                     return QStringLiteral("SNOW");
    case 80: case 81: case 82:   return QStringLiteral("SHOWERS");
    case 85: case 86:            return QStringLiteral("SNOW SHOWERS");
    case 79:                     return QStringLiteral("SLEET");
    case 89:                     return QStringLiteral("HAIL");
    case 95: case 96: case 99:   return QStringLiteral("T'STORMS");
    // Only reachable if a station code ever reaches these columns; the forecast
    // is Open-Meteo's, which never emits the private band.
    case kCodeMostlyCloudy:      return QStringLiteral("CLOUDY");
    default: break;
    }
    return QStringLiteral("UNKNOWN");
}

// Asset name in modules/weather/assets/images/wx/, without the .svg.
//
// Named by what the icon depicts rather than by code, because the icon set is
// far smaller than the code set — 61/63/65 are one rain picture, and three
// files named for the codes would be three identical assets that drift apart
// the first time one is edited.
//
// Only the four sky states take -day/-night variants: they are the ones with a
// sun or moon in them. Rain looks the same at midnight.
//
// Returns empty for anything unmapped, which the views treat as "draw no icon"
// rather than rendering a broken image.
static inline QString iconForCode(int code, bool isDay) {
    const auto sky = [isDay](const char *base) {
        return QString::fromLatin1(base) + (isDay ? QStringLiteral("-day")
                                                  : QStringLiteral("-night"));
    };
    switch (code) {
    case 0:                      return sky("clear");
    case 1:                      return sky("mainly-clear");
    case 2:                      return sky("partly-cloudy");
    case kCodeMostlyCloudy:      return sky("mostly-cloudy");
    case 3:                      return QStringLiteral("overcast");
    case 45: case 48:            return QStringLiteral("fog");
    case 51: case 53: case 55:   return QStringLiteral("drizzle");
    case 56: case 57:            return QStringLiteral("freezing-drizzle");
    case 61: case 63: case 65:   return QStringLiteral("rain");
    case 66: case 67:            return QStringLiteral("freezing-rain");
    case 71: case 73: case 75:   return QStringLiteral("snow");
    case 77:                     return QStringLiteral("snow");
    case 79:                     return QStringLiteral("sleet");
    case 80: case 81: case 82:   return QStringLiteral("showers");
    case 85: case 86:            return QStringLiteral("snow-showers");
    case 89:                     return QStringLiteral("hail");
    case 95:                     return QStringLiteral("thunderstorm");
    case 96: case 99:            return QStringLiteral("thunderstorm-hail");
    default: break;
    }
    return {};
}

// ── METAR → code ─────────────────────────────────────────────────────────────
//
// A station reports sky condition and present weather as two independent
// fields, which is the distinction WMO 4677 throws away. Collapsing them here
// rather than in the view keeps one code domain for both data sources.

// Cloud amount ordered by how much sky is covered, so the worst layer wins.
// Plain code order won't do it: the private 103 sorts above 3 numerically but
// sits below OVERCAST in coverage.
static inline int skyRank(int code) {
    switch (code) {
    case 0:                 return 0;
    case 1:                 return 1;
    case 2:                 return 2;
    case kCodeMostlyCloudy: return 3;
    case 3:                 return 4;
    default:                return -1;
    }
}

static inline int codeForSky(const QJsonArray &layers) {
    // No layers at all is a clear sky: METAR omits the group rather than
    // reporting zero coverage.
    int best = 0;
    for (const QJsonValue &v : layers) {
        const QString amount = v.toObject()[QStringLiteral("amount")].toString();
        int code = -1;
        if      (amount == QLatin1String("CLR") || amount == QLatin1String("SKC")) code = 0;
        else if (amount == QLatin1String("FEW")) code = 1;
        else if (amount == QLatin1String("SCT")) code = 2;
        else if (amount == QLatin1String("BKN")) code = kCodeMostlyCloudy;
        // VV is vertical visibility — the sky is obscured, which from the
        // ground looks like and is reported as full cover.
        else if (amount == QLatin1String("OVC") || amount == QLatin1String("VV")) code = 3;
        if (skyRank(code) > skyRank(best)) best = code;
    }
    return best;
}

// null intensity is moderate — METAR marks only the extremes (-RA, +RA).
static inline int byIntensity(const QString &intensity, int light, int moderate, int heavy) {
    if (intensity == QLatin1String("light")) return light;
    if (intensity == QLatin1String("heavy")) return heavy;
    return moderate;
}

// Present weather → code, or -1 when nothing in the group is worth showing and
// the caller should fall back to sky cover.
//
// Note that showers and freezing are `modifier` values rather than distinct
// `weather` values (SHRA is rain + showers, FZRA is rain + freezing), and that
// `inVicinity` marks weather near the field but not at it — VC in the raw
// METAR — which shouldn't be reported as the current condition.
static inline int codeForPresentWeather(const QJsonArray &pw, double visMetres, bool haveVis) {
    int best = -1;
    int bestPriority = -1;

    // A group may list several phenomena (RA BR). Report the most consequential
    // rather than whichever the station happened to encode first.
    const auto consider = [&](int code, int priority) {
        if (code >= 0 && priority > bestPriority) { bestPriority = priority; best = code; }
    };

    bool hasHail = false;
    for (const QJsonValue &v : pw) {
        const QString w = v.toObject()[QStringLiteral("weather")].toString();
        if (w == QLatin1String("hail") || w == QLatin1String("snow_pellets")) hasHail = true;
    }

    for (const QJsonValue &v : pw) {
        const QJsonObject o = v.toObject();
        if (o[QStringLiteral("inVicinity")].toBool()) continue;

        const QString w   = o[QStringLiteral("weather")].toString();
        const QString in  = o[QStringLiteral("intensity")].toString();
        const QString mod = o[QStringLiteral("modifier")].toString();
        const bool freezing = (mod == QLatin1String("freezing"));
        const bool showers  = (mod == QLatin1String("showers"));

        if (w == QLatin1String("thunderstorms")) {
            consider(hasHail ? 96 : 95, 5);
        } else if (w == QLatin1String("drizzle")) {
            consider(freezing ? byIntensity(in, 56, 57, 57)
                              : byIntensity(in, 51, 53, 55),
                     freezing ? 4 : 2);
        } else if (w == QLatin1String("rain")) {
            if (freezing)     consider(byIntensity(in, 66, 67, 67), 4);
            else if (showers) consider(byIntensity(in, 80, 81, 82), 2);
            else              consider(byIntensity(in, 61, 63, 65), 2);
        } else if (w == QLatin1String("snow")) {
            consider(showers ? byIntensity(in, 85, 86, 86)
                             : byIntensity(in, 71, 73, 75), 3);
        } else if (w == QLatin1String("snow_grains")) {
            consider(77, 3);
        } else if (w == QLatin1String("ice_pellets")) {
            consider(79, 3);
        } else if (w == QLatin1String("hail") || w == QLatin1String("snow_pellets")) {
            consider(89, 3);
        } else if (w == QLatin1String("fog")) {
            consider(freezing ? 48 : 45, 1);
        } else if (w == QLatin1String("fog_mist")) {
            // BR covers everything from thin haze to near-zero visibility, and
            // automated stations report it in any humid air. Calling FOG on a
            // muggy evening with ten miles of visibility would be its own
            // version of the bug this exists to fix, so require the visibility
            // to actually be down before it outranks the sky.
            if (haveVis && visMetres < 3000.0) consider(45, 1);
        }
        // Everything else (haze, smoke, dust, squalls, funnel_cloud…) has no
        // WMO code this module displays, and defers to the sky condition.
    }
    return best;
}

// 16-point compass, matching how the original displayed wind.
static inline QString cardinal(double degrees) {
    static const char *points[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    int idx = int(qRound(degrees / 22.5)) % 16;
    if (idx < 0) idx += 16;
    return QString::fromLatin1(points[idx]);
}

} // namespace wxcodes
