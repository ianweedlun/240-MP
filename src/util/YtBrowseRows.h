#pragma once

// Pure helpers for InnerTube "browse" responses (issue #28, plan D: signed-in
// account lists — subscriptions feed, history, watch-later). Browse pages nest
// their rows as "videoRenderer" objects at varying depths (contents[] and
// continuationItems[] chains) and page past the first via
// "continuationItemRenderer" tokens. These functions only walk the JSON —
// the network side lives in YouTubeBackend. Rows map to the module's existing
// row model (videoId/title/channelName/url); browse exposes only RELATIVE
// publish times ("3 days ago"), so publishedAt/publishedMs stay empty —
// account pages arrive in the API's own order and are never re-sorted.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QVariantList>
#include <QQueue>
#include <QString>

namespace ytui {

// "runs"[0].text or simpleText — the two text shapes InnerTube uses
// interchangeably across clients. Empty when neither is present.
inline QString textOf(const QJsonValue &node)
{
    if (!node.isObject())
        return QString();
    const QJsonObject o = node.toObject();
    const QJsonValue runs = o.value(QStringLiteral("runs"));
    if (runs.isArray() && runs.toArray().size() > 0) {
        const QJsonValue first = runs.toArray().at(0);
        if (first.isObject())
            return first.toObject().value(QStringLiteral("text")).toString();
    }
    return o.value(QStringLiteral("simpleText")).toString();
}

// Collect videoRenderer rows in document order (API order = display order).
// maxRows <= 0 means no cap. A renderer without a videoId is not a row and
// is skipped; missing text fields map to empty strings, never abort the walk.
inline QVariantList extractBrowseRows(const QJsonObject &root, int maxRows)
{
    QVariantList rows;
    QQueue<QJsonValue> queue;
    queue.enqueue(QJsonValue(root));
    while (!queue.isEmpty()) {
        const QJsonValue v = queue.dequeue();
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.contains(QStringLiteral("videoRenderer"))) {
                const QJsonObject r = o.value(QStringLiteral("videoRenderer")).toObject();
                const QString videoId = r.value(QStringLiteral("videoId")).toString();
                if (!videoId.isEmpty()) {
                    QVariantMap row;
                    row[QStringLiteral("videoId")]     = videoId;
                    row[QStringLiteral("title")]       = textOf(r.value(QStringLiteral("title")));
                    QString channelName                = textOf(r.value(QStringLiteral("longBylineText")));
                    if (channelName.isEmpty())
                        channelName = textOf(r.value(QStringLiteral("ownerText")));
                    row[QStringLiteral("channelName")] = channelName;
                    row[QStringLiteral("channelId")]   = QString();
                    row[QStringLiteral("url")]         = QStringLiteral("https://www.youtube.com/watch?v=") + videoId;
                    row[QStringLiteral("publishedText")] = textOf(r.value(QStringLiteral("publishedTimeText")));
                    row[QStringLiteral("lengthText")]  = textOf(r.value(QStringLiteral("lengthText")));
                    row[QStringLiteral("publishedAt")] = QString();
                    row[QStringLiteral("publishedMs")] = qint64(0);
                    row[QStringLiteral("isShort")]     = false;
                    rows << row;
                    if (maxRows > 0 && rows.size() >= maxRows)
                        return rows;
                }
            }
            const QList<QString> keys = o.keys();
            for (const QString &key : keys)
                queue.enqueue(o.value(key));
        } else if (v.isArray()) {
            const QJsonArray arr = v.toArray();
            for (const QJsonValue &item : arr)
                queue.enqueue(item);
        }
    }
    return rows;
}

// First continuationItemRenderer token at any depth — the cursor for the
// next browse page; empty when the list is single-page.
inline QString continuationToken(const QJsonObject &root)
{
    QQueue<QJsonValue> queue;
    queue.enqueue(QJsonValue(root));
    while (!queue.isEmpty()) {
        const QJsonValue v = queue.dequeue();
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.contains(QStringLiteral("continuationItemRenderer"))) {
                const QJsonValue t = o.value(QStringLiteral("continuationItemRenderer"));
                const QJsonValue endpoint = t.toObject().value(QStringLiteral("continuationEndpoint"));
                const QJsonValue command  = endpoint.toObject().value(QStringLiteral("continuationCommand"));
                const QString token       = command.toObject().value(QStringLiteral("token")).toString();
                if (!token.isEmpty())
                    return token;
            }
            const QList<QString> keys = o.keys();
            for (const QString &key : keys)
                queue.enqueue(o.value(key));
        } else if (v.isArray()) {
            const QJsonArray arr = v.toArray();
            for (const QJsonValue &item : arr)
                queue.enqueue(item);
        }
    }
    return QString();
}

} // namespace ytui
