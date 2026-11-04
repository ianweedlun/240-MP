#pragma once
// Pure SponsorBlock -> mpv EDL logic for the 240-MP YouTube module.
// Segment lookup semantics port-adjacent from keshavbhatt/red (GPL-3.0-or-later)
// sponsorblock / sponsorblock_client classes: k-anonymous 4-hex SHA-256 prefix of
// the videoId sent to sponsor.ajay.app, exact-videoUID collision filtering,
// action-type normalization, "full"=[0,0] dead-segment guard. mpv EDL grammar per
// mpv DOCS/edl-mpv.rst (mpv-player/mpv, also GPL-3.0): range segments chain into
// one virtual timeline; skip = ranges omitted; mark = boundary cuts whose
// segment "title" param becomes an OSC chapter title. See THANKS.md.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QString>
#include <QVector>
#include <QPair>
#include <QSet>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonObject>
#include <algorithm>

namespace yt {

struct Segment {
    QString category; // sponsor, selfpromo, intro, outro, intermission, preview, filler, interaction
    QString action;   // skip | mute | full
    double start = -1.0;
    double end = -1.0;
};

// Parse a bare array of segment rows ({category, segmentType, segment:[s,e]}).
inline QVector<Segment> segmentsFromRows(const QJsonArray &rows)
{
    QVector<Segment> out;
    for (const QJsonValue &s : rows) {
        if (!s.isObject())
            continue;
        const QJsonObject so = s.toObject();
        const QJsonArray seg = so.value(QStringLiteral("segment")).toArray();
        if (seg.size() < 2)
            continue;
        Segment sg;
        sg.category = so.value(QStringLiteral("category")).toString();
        sg.action = so.value(QStringLiteral("actionType")).toString();
        if (sg.action.isEmpty())
            sg.action = QStringLiteral("skip");
        sg.start = seg.at(0).toDouble();
        sg.end = seg.at(1).toDouble();
        if (sg.start < 0.0 || sg.end <= sg.start)
            continue; // dead by design: e.g. "full" = [0,0] (Red guard)
        out.append(sg);
    }
    return out;
}

// Parse one "skipSegments" response array for a hash-prefix collision group,
// keeping only rows for the exact videoId (Red collision doctrine), dropping
// degenerate ranges ("full" arrives as [0,0]) and sorting by start.
inline QVector<Segment> sponsorSegmentsFromJson(const QJsonArray &payload,
                                                const QString &videoId)
{
    QVector<Segment> out;
    for (const QJsonValue &groupV : payload) {
        if (!groupV.isObject())
            continue;
        const QJsonObject group = groupV.toObject();
        if (group.value(QStringLiteral("videoID")).toString() != videoId)
            continue;
        out += segmentsFromRows(group.value(QStringLiteral("segments")).toArray());
    }
    std::sort(out.begin(), out.end(), [](const Segment &a, const Segment &b) {
        return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    return out;
}

// Merge ranges whose action is in `drop` (skip and mute both cut the video at
// spawn time; mute-with-audio-kept is not representable in the pair EDL, hence
// Red-parity visual cut). Caller pre-filters by category.
inline QVector<QPair<double, double>> mergeRanges(const QVector<Segment> &segs,
                                                  const QSet<QString> &drop)
{
    QVector<QPair<double, double>> in;
    for (const Segment &s : segs)
        if (drop.contains(s.action))
            in.append(QPair<double, double>(s.start, s.end));
    std::sort(in.begin(), in.end(),
              [](const QPair<double, double> &a, const QPair<double, double> &b) {
                  return a.first != b.first ? a.first < b.first : a.second < b.second;
              });
    QVector<QPair<double, double>> out;
    for (const auto &r : in) {
        if (!out.isEmpty() && r.first <= out.last().second)
            out.last().second = qMax(out.last().second, r.second);
        else
            out.append(r);
    }
    return out;
}

// mpv 0.40 requires the %<len>% prefix on EVERY EDL file field (even a comma-free
// https:// URL fails "Missing filename in segment" unescaped), but plain form is
// fine for short option values like titles. Always escape files; escape other
// values only when they carry , ; \n or !.
inline QString edlFile(const QString &v)
{
    return QStringLiteral("%") + QString::number(v.size()) + QStringLiteral("%") + v;
}
inline QString edlValue(const QString &v)
{
    for (const QChar &c : v) {
        if (c == u',' || c == u';' || c == u'!' || c == u'\n')
            return QStringLiteral("%") + QString::number(v.size()) + QStringLiteral("%") + v;
    }
    return v;
}

// Compact time formatting: 5 -> "5", 7.5 -> "7.5", 15.25 -> "15.25".
inline QString fmtTime(double t)
{
    QString s = QString::number(t, 'f', 3);
    if (s.contains(u'.')) {
        while (s.endsWith(u'0'))
            s.chop(1);
        if (s.endsWith(u'.'))
            s.chop(1);
    }
    return s;
}

// Build the full edl:// URI for one media URL: `skip` merged cut ranges (omitted
// from the virtual timeline), `marks` boundary cuts whose following segment
// carries the category as chapter title. Empty QString = nothing to change.
inline QString buildEdl(const QString &url,
                        const QVector<QPair<double, double>> &skip,
                        const QVector<Segment> &marks)
{
    if (skip.isEmpty() && marks.isEmpty())
        return QString();

    auto inSkip = [&skip](double t) {
        for (const auto &r : skip)
            if (t >= r.first && t <= r.second)
                return true;
        return false;
    };
    auto markTitle = [&marks](double segStart) {
        for (const Segment &m : marks)
            if (segStart >= m.start - 1e-6 && segStart < m.end - 1e-6)
                return m.category;
        return QString();
    };

    QSet<double> ptSet;
    for (const auto &r : skip) {
        if (r.first > 1e-6)
            ptSet.insert(r.first);
        ptSet.insert(r.second);
    }
    for (const Segment &m : marks) {
        if (m.start > 1e-6)
            ptSet.insert(m.start);
        ptSet.insert(m.end);
    }
    QVector<double> points(ptSet.begin(), ptSet.end());
    std::sort(points.begin(), points.end());

    QString body;
    bool titles = false;
    double pos = 0.0;
    auto emitSeg = [&](double a, double b, bool open) {
        if (inSkip(a + 0.001))
            return;
        const QString title = markTitle(a);
        if (!title.isEmpty())
            titles = true;
        if (!open && b - a <= 1e-9)
            return;
        if (!body.isEmpty())
            body += u';';
        body += edlFile(url);
        const bool hasStart = a > 1e-9;
        if (open) {
            if (hasStart)
                body += u',' + fmtTime(a);
        } else {
            if (hasStart)
                body += u',' + fmtTime(a) + u',' + fmtTime(b - a);
            else
                body += u',' + fmtTime(0.0) + u',' + fmtTime(b - a);
        }
        if (!title.isEmpty())
            body += QStringLiteral(",title=") + edlValue(title);
    };
    for (double p : points) {
        if (p > pos) {
            emitSeg(pos, p, false);
            pos = p;
        }
    }
    if (!(points.isEmpty() && pos <= 1e-9 && skip.isEmpty()))
        emitSeg(pos, 0.0, true); // open-ended tail (rest of file)

    if (body.isEmpty())
        return QString();
    const QString head = titles ? QString() : QStringLiteral("!no_chapters;");
    return QStringLiteral("edl://") + head + body;
}

} // namespace yt
