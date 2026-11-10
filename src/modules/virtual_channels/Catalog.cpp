#include "Catalog.h"

#include <QJsonArray>

namespace vchan {

namespace {

// TheTVDB returns entity ids as JSON integers in the v4 payloads, but a couple
// of endpoints stringify them. Normalise to a string so the id stored on the
// Subject (and later matched against a server field) is stable either way.
QString asId(const QJsonValue &v) {
    if (v.isDouble()) {
        // Exact for the id ranges we deal with; ids are small positive ints.
        return QString::number(v.toInt());
    }
    return v.toString();
}

} // namespace

QVector<Subject> Catalog::parseGenres(const QJsonObject &body) {
    QVector<Subject> out;
    for (const QJsonValue &v : body.value(QStringLiteral("data")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) continue;
        Subject s;
        s.kind = SubjectKind::Genre;
        s.id = asId(o.value(QStringLiteral("id")));
        s.name = name;
        out.append(s);
    }
    return out;
}

QVector<Subject> Catalog::parseNetworks(const QJsonObject &companies,
                                        const QJsonObject &types) {
    // companyTypeId -> companyTypeName, e.g. {1, "network"}, {2, "studio"}.
    QHash<int, QString> typeNames;
    for (const QJsonValue &v : types.value(QStringLiteral("data")).toArray()) {
        const QJsonObject t = v.toObject();
        typeNames.insert(t.value(QStringLiteral("companyTypeId")).toInt(),
                         t.value(QStringLiteral("companyTypeName")).toString());
    }

    QVector<Subject> out;
    for (const QJsonValue &v : companies.value(QStringLiteral("data")).toArray()) {
        const QJsonObject c = v.toObject();
        const int typeId = c.value(QStringLiteral("primaryCompanyType")).toInt(-1);
        if (typeNames.value(typeId) != QStringLiteral("network")) continue;

        const QString name = c.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) continue;
        Subject s;
        s.kind = SubjectKind::Network;
        s.id = asId(c.value(QStringLiteral("id")));
        s.name = name;
        out.append(s);
    }
    return out;
}

} // namespace vchan
