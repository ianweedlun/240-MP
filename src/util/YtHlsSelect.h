#pragma once

// Live HLS master-playlist shaping (#128). YouTube live events expose only
// HLS renditions (video-only variants plus separate audio groups); the
// progressive pair picker must never serve them — a live "pair" launched mpv
// into "Failed to recognize file format" on the owner's CRT. This pure helper
// rewrites a master playlist down to variants at or below the resolution cap
// so mpv's ffmpeg demuxer plays ONE HLS stream with internal A/V sync,
// instead of the app juggling two independent live playlists that can drift.
//
// Scope: the tags YouTube masters actually carry (#EXTM3U, #EXT-X-VERSION,
// #EXT-X-MEDIA audio renditions, #EXT-X-STREAM-INF + URI). Unknown tags are
// dropped; the rewrite is deliberately minimal, not a general HLS editor.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace ythls {

struct Variant {
    int height = 0;          // from RESOLUTION=WxH; 0 when absent
    int bandwidth = 0;       // from BANDWIDTH; 0 when absent
    QString infLine;         // original #EXT-X-STREAM-INF line, attrs verbatim
    QString uri;             // raw URI line as written
    QString audioGroup;      // AUDIO="group" attr, empty when absent
};

struct MediaRendition {
    QString groupId;         // GROUP-ID attr
    QString tagLine;         // original #EXT-X-MEDIA line
    QString uri;             // URI attr value, empty when absent
};

// Quote-aware comma split: CODECS="avc1.4d400c,mp4a.40.2" must stay one attr.
inline QStringList splitAttrs(const QString &s)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (const QChar c : s) {
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            cur.append(c);
        } else if (c == QLatin1Char(',') && !inQuotes) {
            out.append(cur);
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    if (!cur.isEmpty())
        out.append(cur);
    return out;
}

inline QString attrValue(const QStringList &attrs, const QString &key)
{
    const QString prefix = key + QLatin1Char('=');
    for (const QString &a : attrs) {
        if (a.startsWith(prefix)) {
            QString v = a.mid(prefix.size());
            if (v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')) && v.size() >= 2)
                v = v.mid(1, v.size() - 2);
            return v;
        }
    }
    return QString();
}

inline QString absolutize(const QString &uri, const QUrl &base)
{
    return base.resolved(QUrl(uri)).toString();
}

// Returns the rewritten master playlist, or an empty string when the input is
// not a usable master (no #EXTM3U header, no variant, invalid cap). When no
// variant fits the cap the lowest known resolution still wins — a live event
// must play rather than fail; variants without RESOLUTION fall back to the
// lowest BANDWIDTH. Audio renditions are kept only when a kept variant
// references their GROUP-ID. URIs are absolutized against baseUrl.
inline QString filterMaster(const QString &master, const QUrl &baseUrl, int maxHeight)
{
    if (maxHeight <= 0 || !baseUrl.isValid())
        return QString();

    QString body = master;
    if (body.startsWith(QChar(0xFEFF)))
        body.remove(0, 1);

    QVector<Variant> variants;
    QVector<MediaRendition> renditions;
    QString versionLine;
    bool sawHeader = false;
    bool pendingInf = false;
    Variant pending;

    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('#'))) {
            if (line == QLatin1String("#EXTM3U")) {
                sawHeader = true;
            } else if (line.startsWith(QLatin1String("#EXT-X-VERSION:")) && versionLine.isEmpty()) {
                versionLine = line;
            } else if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
                pendingInf = true;
                pending = Variant();
                pending.infLine = line;
                const QStringList attrs = splitAttrs(line.mid(QStringLiteral("#EXT-X-STREAM-INF:").size()));
                const QString res = attrValue(attrs, QStringLiteral("RESOLUTION"));
                const int x = res.indexOf(QLatin1Char('x'));
                if (x > 0)
                    pending.height = res.mid(x + 1).toInt();
                pending.bandwidth = attrValue(attrs, QStringLiteral("BANDWIDTH")).toInt();
                pending.audioGroup = attrValue(attrs, QStringLiteral("AUDIO"));
            } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))) {
                const QStringList attrs = splitAttrs(line.mid(QStringLiteral("#EXT-X-MEDIA:").size()));
                if (attrValue(attrs, QStringLiteral("TYPE")) == QLatin1String("AUDIO")) {
                    MediaRendition r;
                    r.groupId = attrValue(attrs, QStringLiteral("GROUP-ID"));
                    r.tagLine = line;
                    r.uri = attrValue(attrs, QStringLiteral("URI"));
                    renditions.append(r);
                }
            }
            continue;
        }
        // Non-comment line: the URI of a pending STREAM-INF.
        if (pendingInf) {
            pending.uri = line;
            variants.append(pending);
            pendingInf = false;
        }
    }

    if (!sawHeader || variants.isEmpty())
        return QString();

    // Eligible: known height at or below the cap.
    QVector<const Variant *> kept;
    for (const Variant &v : variants)
        if (v.height > 0 && v.height <= maxHeight)
            kept.append(&v);

    if (kept.isEmpty()) {
        // Fallback 1: lowest known resolution regardless of cap.
        const Variant *best = nullptr;
        for (const Variant &v : variants) {
            if (v.height <= 0)
                continue;
            if (!best || v.height < best->height)
                best = &v;
        }
        // Fallback 2: no RESOLUTION anywhere — lowest BANDWIDTH.
        if (!best) {
            for (const Variant &v : variants) {
                if (v.bandwidth <= 0)
                    continue;
                if (!best || v.bandwidth < best->bandwidth)
                    best = &v;
            }
        }
        if (!best)
            return QString();
        kept.append(best);
    }

    QSet<QString> usedGroups;
    for (const Variant *v : kept)
        if (!v->audioGroup.isEmpty())
            usedGroups.insert(v->audioGroup);

    QString out = QStringLiteral("#EXTM3U\n");
    if (!versionLine.isEmpty())
        out += versionLine + QLatin1Char('\n');
    static const QRegularExpression uriAttr(QStringLiteral("URI=\"[^\"]*\""));
    for (const MediaRendition &r : renditions) {
        if (!usedGroups.contains(r.groupId))
            continue;
        QString tag = r.tagLine;
        if (!r.uri.isEmpty())
            tag.replace(uriAttr, QStringLiteral("URI=\"%1\"").arg(absolutize(r.uri, baseUrl)));
        out += tag + QLatin1Char('\n');
    }
    for (const Variant *v : kept) {
        out += v->infLine + QLatin1Char('\n');
        out += absolutize(v->uri, baseUrl) + QLatin1Char('\n');
    }
    return out;
}

} // namespace ythls
