QList<YouTubeBackend::PlaylistFileRef> YouTubeBackend::readPlaylistEntries(QString *error) const {
    const QString path = m_dataRoot + "/" + kPlaylistsFileName;
    if (!QFile::exists(path)) {
        if (error)
            *error = QStringLiteral("NO PLAYLISTS FILE FOUND\n"
                                    "CREATE YOUTUBE_PLAYLISTS.TXT IN THE DATA DIRECTORY\n"
                                    "WITH ONE PLAYLIST URL PER LINE");
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("COULD NOT READ YOUTUBE_PLAYLISTS.TXT");
        return {};
    }
    QList<PlaylistFileRef> refs;
    QStringList seen;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        // Split the optional display-name prefix at the last '|' (URLs never
        // contain one, display names conceivably could).
        QString name, token = line;
        const int bar = line.lastIndexOf('|');
        if (bar >= 0) {
            name  = line.left(bar).trimmed();
            token = line.mid(bar + 1).trimmed();
        }
        const QString id = playlistIdFromToken(token);
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen << id;
        refs.append({id, name});
    }
    if (refs.isEmpty() && error)
        *error = QStringLiteral("NO PLAYLISTS FOUND IN YOUTUBE_PLAYLISTS.TXT");
    return refs;
}
