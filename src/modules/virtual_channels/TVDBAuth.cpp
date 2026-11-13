#include "TVDBAuth.h"

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
    return key.isEmpty() ? QString() : key;
}

bool TVDBAuth::setApikey(const QString &apikey) {
    if (apikey.isEmpty()) return false;

    QFileInfo info(m_path);
    // Create the parent directory (and any missing ancestors) if needed.
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    f.write(apikey.toUtf8());
    f.flush();
    f.close();

    // Owner read/write only — never world-readable. Matches PlexBackend.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace vchan
