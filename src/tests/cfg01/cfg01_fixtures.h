#pragma once
// CFG-01 gate fixtures — single source of truth (test plan §8 D-4), shared by
// saver, classifier, kill-sweep orchestrator and harness. Synthetic credentials
// only (FIXTURE-TOKEN-123). No network.
//
// F1 = realistic multi-module fixture (the torture armA shape).
// F2 = bigFixture: F1-shaped doc padded to 68,410 B so the truncation window
//      is wide enough to hit with a µs kill clock.
// F1-CUT = F1 truncated mid-token (467 B) — the power-cut corpse shape.
//
// Pinned hashes (test plan §4.3, durable evidence):
//   F2 template   68410 B  c7bf857d97a895151d73c13eadd35a184998f8bbb12d549651934b03cd1888e1
//   GV3 RLIMIT cut 1024 B  62326a9001587c54dbd97b04c9972ca4b939193a59c187a0d2c7af27b9e38d59
//   F1-CUT          467 B  a1535fcdc5fd5424a629d98c33081ee3bbcce413c302ab1223562c977ecae009
//   PRE/P2 wipe     152 B  02f732d69a1bce75ccee5de40f464d8f5077780dde91199cddd8dc6a2e063a23

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <cstdio>

namespace cfg01 {

inline constexpr int kF2Size = 68410;
inline constexpr auto kF2Sha =
    "c7bf857d97a895151d73c13eadd35a184998f8bbb12d549651934b03cd1888e1";
inline constexpr auto kGv3Sha =
    "62326a9001587c54dbd97b04c9972ca4b939193a59c187a0d2c7af27b9e38d59";
inline constexpr int kF1CutSize = 467;
inline constexpr auto kF1CutSha =
    "a1535fcdc5fd5424a629d98c33081ee3bbcce413c302ab1223562c977ecae009";
inline constexpr auto kWipeSha =
    "02f732d69a1bce75ccee5de40f464d8f5077780dde91199cddd8dc6a2e063a23";

// F1 — verbatim from the inherited torture harness (byte-stable).
inline QJsonObject fixtureObject()
{
    QJsonObject plex{
        {"enabled", true},
        {"token", "FIXTURE-TOKEN-123"},
        {"server_url", "http://127.0.0.1:32400"},
        {"libraries", QJsonObject{{"movies", "1"}, {"tv", "2"}}}};
    QJsonObject weather{
        {"enabled", true}, {"units", "imperial"}, {"location", "New York, NY"}};
    QJsonObject local{
        {"enabled", true}, {"media_directory", "/home/fixture/Media"}};
    QJsonObject retro{
        {"enabled", true}, {"playlist_sort", "SYSTEM"},
        {"rom_directory", "/home/fixture/ROMs"}};
    QJsonObject yt{{"enabled", false}, {"resolution", "720p"}};
    QJsonObject modules{
        {"com.240mp.plex", plex},
        {"com.240mp.weather", weather},
        {"com.240mp.local_files", local},
        {"com.240mp.retroarch", retro},
        {"com.240mp.youtube", yt}};
    return QJsonObject{
        {"app", QJsonObject{{"color_scheme", "Video 2"}, {"idle_timeout", "300"}}},
        {"modules", modules}};
}

inline QByteArray fixtureBytes()
{
    return QJsonDocument(fixtureObject()).toJson(QJsonDocument::Indented);
}

// Cut inside the plex token string — exactly what a power cut mid-write of
// this document leaves (open() truncated, only the first N bytes written).
inline int cutOffset(const QByteArray &b)
{
    int i = b.indexOf("FIXTURE-TOKEN-123");
    return i < 0 ? b.size() / 2 : i + 5;
}

inline QByteArray fixtureCutBytes()
{
    const QByteArray full = fixtureBytes();
    return full.left(cutOffset(full));
}

// F2 — verbatim bigFixture from the inherited killprobe saver (byte-stable:
// 2000 pad keys, 68,410 B Indented).
inline QJsonObject bigFixture()
{
    QJsonObject pad;
    for (int i = 0; i < 2000; ++i) {
        char k[16], v[16];
        snprintf(k, sizeof k, "k%04d", i);
        snprintf(v, sizeof v, "v%04d", i);
        pad.insert(QString::fromLatin1(k), QString::fromLatin1(v));
    }
    QJsonObject plex{
        {"enabled", true},
        {"token", "FIXTURE-TOKEN-123"},
        {"server_url", "http://127.0.0.1:32400"},
        {"pad", pad}};
    QJsonObject weather{{"enabled", true}, {"units", "imperial"}};
    QJsonObject modules{{"com.240mp.plex", plex}, {"com.240mp.weather", weather}};
    return QJsonObject{
        {"app", QJsonObject{{"color_scheme", "Video 2"}, {"idle_timeout", "300"}}},
        {"modules", modules}};
}

inline QByteArray bigFixtureBytes()
{
    return QJsonDocument(bigFixture()).toJson(QJsonDocument::Indented);
}

// Intended post-save doc for the kill sweep: the saver drives
// save_setting("", "color_scheme", "Video 3") — same value length, so a
// complete write is size-identical to the template (§4.2).
inline QJsonObject intendedObject()
{
    QJsonObject o = bigFixture();
    QJsonObject app = o.value("app").toObject();
    app["color_scheme"] = "Video 3";
    o["app"] = app;
    return o;
}

inline QByteArray intendedBytes()
{
    return QJsonDocument(intendedObject()).toJson(QJsonDocument::Indented);
}

// Structural guard — mirrors cfgfile::passesGuard EXACTLY (decision D2).
// Duplicated here because Tier-1 instruments must also compile against the
// PRE/P2 snapshots, which have no ConfigFile.cpp. The Tier-2 policy unit
// asserts this mirror == cfgfile::passesGuard over the whole vector set.
inline bool passesGuard(const QJsonObject &doc)
{
    const bool hasApp = doc.contains(QStringLiteral("app"));
    const bool hasModules = doc.contains(QStringLiteral("modules"));
    if (!hasApp && !hasModules)
        return false;   // {} — the stale-empty-doc incident shape
    if (hasApp && !doc.value(QStringLiteral("app")).isObject())
        return false;
    if (hasModules && !doc.value(QStringLiteral("modules")).isObject())
        return false;
    return true;
}

} // namespace cfg01