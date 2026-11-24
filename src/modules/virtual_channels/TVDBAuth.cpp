#include "TVDBAuth.h"

#include "../../util/CredentialFile.h"

#include <QDebug>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QDir>

namespace vchan {

TVDBAuth::TVDBAuth(const QString &tokenFilePath) : m_path(tokenFilePath) {}

QString TVDBAuth::defaultPath(const QString &dataRoot) {
    return dataRoot + QStringLiteral("/thetvdb_apikey");
}

QString TVDBAuth::apikey() const {
    QFile f(m_path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    // First line only; tolerate trailing newline / surrounding whitespace.
    QByteArray raw = f.readLine();
    QString key = QString::fromUtf8(raw).trimmed();
    if (key.isEmpty() && f.size() > 0) {
        // CFG-01 N4a: a zero/whitespace-only key file is damage, not a first
        // run. It is left byte-identical; the caller can supply a new key.
        qWarning("[TVDBAuth] thetvdb_apikey is empty/corrupt at %s",
                 qPrintable(m_path));
    }
    return key.isEmpty() ? QString() : key;
}

bool TVDBAuth::setApikey(const QString &apikey) {
    if (apikey.isEmpty()) return false;

    QFileInfo info(m_path);
    // Create the parent directory (and any missing ancestors) if needed.
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    // CFG-01 N4a: atomic write with checked returns, file+dir fsync, 0600.
    // allowCorrupt=true: this is an EXPLICIT user-supplied replacement (the
    // silent-defaults case does not apply), but credfile still warns loudly
    // and leaves the old file byte-identical should the write fail.
    return credfile::write(m_path, apikey.toUtf8(),
                           credfile::Shape::ApiKeyLine, "thetvdb_apikey",
                           /*allowCorrupt=*/true);
}

} // namespace vchan
