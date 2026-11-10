#ifndef VCHAN_TVDB_AUTH_H
#define VCHAN_TVDB_AUTH_H

#include <QString>

namespace vchan {

// Secure home for the 240mp user's own TheTVDB v4 apikey. The key is NEVER
// baked into the app: it lives in a 0600 file in the user's data directory and
// is passed as a parameter to the resolver that needs it (PlexBackend's
// plex_auth.json pattern — setPermissions(ReadOwner | WriteOwner)).
//
//   TVDBAuth auth(dataRoot + "/thetvdb_apikey");
//   QString key = auth.apikey();          // "" if unset / unreadable
//   auth.setApikey(userSuppliedKey);      // writes 0600
class TVDBAuth {
public:
    explicit TVDBAuth(const QString &tokenFilePath);

    // The stored apikey, or an empty string if the file is absent/unreadable.
    QString apikey() const;

    // Persist the apikey with owner-only (0600) permissions. Returns false if
    // the file cannot be written.
    bool setApikey(const QString &apikey);

    bool hasApikey() const { return !apikey().isEmpty(); }

    // The conventional location: <dataRoot>/thetvdb_apikey.
    static QString defaultPath(const QString &dataRoot);

private:
    QString m_path;
};

} // namespace vchan

#endif // VCHAN_TVDB_AUTH_H
