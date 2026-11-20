#pragma once

// Subscription-union dedup (#128). The aggregate feed concatenates every
// channel's RSS with no dedup; YouTube carries the same broadcast under two
// video ids when a streamer double-starts a live event (measured on the
// owner's CRT: "KNICKS VS TIMBERWOLVES WATCH PARTY!!" twice, both is_live,
// same channel), and cross-channel collabs repeat one videoId. Collapse:
//   - identical videoId anywhere in the union (newest publication wins);
//   - same channel + identical title published within kLiveDuplicateWindowMs
//     (a live double-post). Far-apart same-title uploads (daily shows with a
//     fixed name) stay distinct — the window is the safety margin.
// Rows must arrive newest-first (buildFeed sorts before calling); output
// preserves input order. Truncation stays the caller's job.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QMap>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace ytfeed {

// Live double-posts land minutes apart; a scheduled event's waiting-room row
// can precede the live row by a few hours. 12 h covers both while staying
// well under the 24 h cadence of same-title daily uploads.
inline constexpr qint64 kLiveDuplicateWindowMs = 12 * 60 * 60 * 1000;

inline QVariantList dedupFeed(const QVariantList &rows)
{
    QVariantList out;
    QSet<QString> seenIds;
    QMap<QString, qint64> titleNewest;   // channel\x1ftitle -> kept publishedMs

    for (const QVariant &rv : rows) {
        const QVariantMap v = rv.toMap();
        const QString id = v.value(QStringLiteral("videoId")).toString();
        if (!id.isEmpty() && seenIds.contains(id))
            continue;

        const QString channel = v.value(QStringLiteral("channelId")).toString();
        const QString title = v.value(QStringLiteral("title")).toString();
        const qint64 published = v.value(QStringLiteral("publishedMs")).toLongLong();

        bool collapse = false;
        if (!channel.isEmpty() && !title.isEmpty() && published > 0) {
            const QString key = channel + QChar(0x1f) + title;
            const auto it = titleNewest.constFind(key);
            if (it != titleNewest.constEnd() && it.value() > 0
                && qAbs(published - it.value()) <= kLiveDuplicateWindowMs) {
                collapse = true;
            } else {
                // Track the newest kept publication so a third row close to
                // this one also collapses (chain), while far-apart rows stay.
                titleNewest.insert(key, published);
            }
        }
        if (collapse)
            continue;

        if (!id.isEmpty())
            seenIds.insert(id);
        out.append(rv);
    }
    return out;
}

} // namespace ytfeed
