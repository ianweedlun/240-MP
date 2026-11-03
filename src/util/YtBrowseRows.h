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

// First plain text line of a tile's metadata block: the channel name. Badged
// items (4K etc.) and view/watched counters are NOT the owner — skip them.
inline QString tileOwnerOf(const QJsonObject &meta)
{
    const QJsonValue lines = meta.value(QStringLiteral("lines"));
    if (!lines.isArray())
        return QString();
    for (const QJsonValue &line : lines.toArray()) {
        const QJsonValue itemsInWrapper = line.isObject()
                ? line.toObject().value(QStringLiteral("items"))
                : QJsonValue();
        const QJsonValue lineRenderer = line.isObject()
                ? line.toObject().value(QStringLiteral("lineRenderer"))
                : QJsonValue();
        const QJsonValue items = itemsInWrapper.isArray()
                ? itemsInWrapper
                : (lineRenderer.isObject() ? lineRenderer.toObject().value(QStringLiteral("items"))
                                           : QJsonValue());
        if (!items.isArray())
            continue;
        for (const QJsonValue &item : items.toArray()) {
            if (!item.isObject())
                continue;
            const QJsonObject itemWrap = item.toObject();
            // items nest one more level: {lineItemRenderer:{text|badge,…}}
            const QJsonValue inner = itemWrap.value(QStringLiteral("lineItemRenderer"));
            const QJsonObject li = inner.isObject() ? inner.toObject() : itemWrap;
            if (li.contains(QStringLiteral("badge")))
                continue;
            const QString text = textOf(li.value(QStringLiteral("text")));
            if (text.isEmpty())
                continue;
            if (text.endsWith(QStringLiteral(" views"))
                || text.endsWith(QStringLiteral(" watched"))
                || text == QStringLiteral("Shared"))
                continue;
            return text;
        }
    }
    return QString();
}

// Duration shown as a time-status overlay on the tile header thumbnail
// ("3:41" or "STREAMING" for live); empty when absent.
inline QString tileLengthOf(const QJsonObject &header)
{
    const QJsonValue overlays = header.value(QStringLiteral("thumbnailOverlays"));
    if (!overlays.isArray())
        return QString();
    for (const QJsonValue &ov : overlays.toArray()) {
        if (!ov.isObject())
            continue;
        const QJsonValue t = ov.toObject().value(QStringLiteral("thumbnailOverlayTimeStatusRenderer"));
        if (t.isObject())
            return textOf(t.toObject().value(QStringLiteral("text")));
    }
    return QString();
}

// Collect videoRenderer rows in document order (API order = display order).
// maxRows <= 0 means no cap. A renderer without a videoId is not a row and
// is skipped; missing text fields map to empty strings, never abort the walk.
// TV-client (TVHTML5) browse pages do NOT carry videoRenderer at all — the
// verified account pages nest tileRenderer: contentId + contentType
// TILE_CONTENT_TYPE_VIDEO are the identity, the title lives in
// metadata.tileMetadataRenderer.title (simpleText), the owner line is the
// first lineItemRenderer text that is not a badge/view-count, and the
// duration is a thumbnailOverlayTimeStatusRenderer on the header thumbnail.
// Both shapes are handled so a client flip never breaks either parser path.
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
            if (o.contains(QStringLiteral("tileRenderer"))) {
                const QJsonObject t = o.value(QStringLiteral("tileRenderer")).toObject();
                const QString videoId = t.value(QStringLiteral("contentId")).toString();
                const QString ctype   = t.value(QStringLiteral("contentType")).toString();
                // Only actual video tiles are rows (channels/playlists share the
                // tile shape with different contentType). Empty ctype accepted
                // defensively for older TV payloads observed without the field.
                if (!videoId.isEmpty()
                    && (ctype.isEmpty() || ctype == QStringLiteral("TILE_CONTENT_TYPE_VIDEO"))) {
                    QVariantMap row;
                    row[QStringLiteral("videoId")]     = videoId;
                    const QJsonObject meta = t.value(QStringLiteral("metadata")).toObject()
                                                 .value(QStringLiteral("tileMetadataRenderer")).toObject();
                    row[QStringLiteral("title")]       = textOf(meta.value(QStringLiteral("title")));
                    row[QStringLiteral("channelName")] = tileOwnerOf(meta);
                    row[QStringLiteral("channelId")]   = QString();
                    row[QStringLiteral("url")]         = QStringLiteral("https://www.youtube.com/watch?v=") + videoId;
                    row[QStringLiteral("publishedText")] = QString(); // TV tiles: no relative publish time
                    const QJsonObject header = t.value(QStringLiteral("header")).toObject()
                                                 .value(QStringLiteral("tileHeaderRenderer")).toObject();
                    row[QStringLiteral("lengthText")]  = tileLengthOf(header);
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

// Watch-later cursor for the signed-in account home page. Two verified shapes
// across clients: the TVHTML5 tab chip keys the title text "Watch later"
// beside content.tvSurfaceContentRenderer.continuation.reloadContinuationData
// (a raw continuation cursor, no browseId anywhere), while desktop shapes
// name a playlistId (PL…, browsed with a VL prefix) or a direct VL browseId.
// Any cursor string is returned as-is — the caller branches on prefix. Null
// when the page carries no match (the caller falls back to the file view).
inline bool subtreeHasWatchLaterText(const QJsonValue &v)
{
    QQueue<QJsonValue> queue;
    queue.enqueue(v);
    while (!queue.isEmpty()) {
        const QJsonValue cur = queue.dequeue();
        if (cur.isString()) {
            const QString s = cur.toString().toLower();
            if (s == QStringLiteral("watch later")
                || s == QStringLiteral("watchlater"))
                return true;
        } else if (cur.isObject()) {
            const QJsonObject o = cur.toObject();
            const QList<QString> keys = o.keys();
            for (const QString &k : keys)
                queue.enqueue(o.value(k));
        } else if (cur.isArray()) {
            const QJsonArray arr = cur.toArray();
            for (const QJsonValue &item : arr)
                queue.enqueue(item);
        }
    }
    return false;
}

inline QJsonValue findWatchLaterCursor(const QJsonObject &root)
{
    auto isWatchLaterTitle = [](const QJsonValue &t) -> bool {
        if (t.isString())
            return t.toString().toLower() == QStringLiteral("watch later");
        if (t.isObject())
            return textOf(t).toLower() == QStringLiteral("watch later");
        return false;
    };
    QQueue<QJsonValue> queue;
    queue.enqueue(QJsonValue(root));
    while (!queue.isEmpty()) {
        const QJsonValue v = queue.dequeue();
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (isWatchLaterTitle(o.value(QStringLiteral("title")))) {
                // TV chip: raw continuation cursor under content.
                const QJsonValue content = o.value(QStringLiteral("content"));
                if (content.isObject()) {
                    const QJsonValue tv = content.toObject().value(
                            QStringLiteral("tvSurfaceContentRenderer"));
                    if (tv.isObject()) {
                        const QJsonValue cont = tv.toObject().value(
                                QStringLiteral("continuation"));
                        if (cont.isObject()) {
                            const QJsonValue rcd = cont.toObject().value(
                                    QStringLiteral("reloadContinuationData"));
                            if (rcd.isObject()) {
                                const QString tok = rcd.toObject().value(
                                        QStringLiteral("continuation")).toString();
                                if (!tok.isEmpty())
                                    return QJsonValue(tok);
                            }
                        }
                    }
                }
                // Desktop: a VL browseId sitting on the chip's endpoint.
                const QJsonValue ep = o.value(QStringLiteral("endpoint"));
                if (ep.isObject()) {
                    const QJsonValue be = ep.toObject().value(
                            QStringLiteral("browseEndpoint"));
                    if (be.isObject()) {
                        const QString bid = be.toObject().value(
                                QStringLiteral("browseId")).toString();
                        if (bid.startsWith(QStringLiteral("VL")))
                            return QJsonValue(bid);
                    }
                }
            }
            // Playlist-named cursors need the subtree text test to bind the
            // chip (bare playlistIds appear everywhere on the page).
            const QString pid      = o.value(QStringLiteral("playlistId")).toString();
            const QString browseId = o.value(QStringLiteral("browseId")).toString();
            QString cursor;
            if (browseId.startsWith(QStringLiteral("VL")))
                cursor = browseId;
            else if (pid.startsWith(QStringLiteral("PL")) || pid.startsWith(QStringLiteral("WL")))
                cursor = QStringLiteral("VL") + pid;
            if (!cursor.isEmpty() && subtreeHasWatchLaterText(v))
                return QJsonValue(cursor);
            const QList<QString> keys = o.keys();
            for (const QString &k : keys)
                queue.enqueue(o.value(k));
        } else if (v.isArray()) {
            const QJsonArray arr = v.toArray();
            for (const QJsonValue &item : arr)
                queue.enqueue(item);
        }
    }
    return QJsonValue(); // null sentinel
}

} // namespace ytui
