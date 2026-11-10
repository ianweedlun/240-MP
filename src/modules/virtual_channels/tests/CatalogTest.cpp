#include "TestHarness.h"
#include "../Catalog.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QVector>

using namespace vchan;

int runCatalogTests() {
    vtest::section("Catalog");

    // --- parseGenres: int ids, name only ---
    {
        QJsonObject body;
        QJsonArray arr;
        arr.append(QJsonObject{{"id", 1}, {"name", "Action"}});
        arr.append(QJsonObject{{"id", 2}, {"name", "Comedy"}});
        body.insert("data", arr);

        auto out = Catalog::parseGenres(body);
        vtest::checkEq(out.size(), 2, "two genres parsed");
        vtest::checkEq(static_cast<int>(out.at(0).kind), static_cast<int>(SubjectKind::Genre), "genre kind");
        vtest::checkStr(out.at(0).id, "1", "genre id");
        vtest::checkStr(out.at(0).name, "Action", "genre name");
        vtest::checkStr(out.at(1).name, "Comedy", "second genre");
    }

    // --- parseGenres: trims, skips empty names ---
    {
        QJsonObject body;
        QJsonArray arr;
        arr.append(QJsonObject{{"id", 3}, {"name", "  Drama  "}});
        arr.append(QJsonObject{{"id", 4}, {"name", ""}});
        body.insert("data", arr);

        auto out = Catalog::parseGenres(body);
        vtest::checkEq(out.size(), 1, "empty-name genre dropped");
        vtest::checkStr(out.at(0).name, "Drama", "whitespace trimmed");
    }

    // --- parseNetworks: keeps only companies typed "network" ---
    {
        QJsonObject types;
        QJsonArray tarr;
        tarr.append(QJsonObject{{"companyTypeId", 1}, {"companyTypeName", "network"}});
        tarr.append(QJsonObject{{"companyTypeId", 2}, {"companyTypeName", "studio"}});
        types.insert("data", tarr);

        QJsonObject companies;
        QJsonArray carr;
        // AMC -> type 1 (network) -> kept
        carr.append(QJsonObject{{"id", 389}, {"primaryCompanyType", 1}, {"name", "AMC"}});
        // HBO -> type 1 (network) -> kept
        carr.append(QJsonObject{{"id", 402}, {"primaryCompanyType", 1}, {"name", "HBO"}});
        // Marvel -> type 2 (studio) -> dropped
        carr.append(QJsonObject{{"id", 627}, {"primaryCompanyType", 2}, {"name", "Marvel"}});
        // untyped (primaryCompanyType missing -> -1, no mapping) -> dropped
        carr.append(QJsonObject{{"id", 9}, {"name", "Mystery Co"}});
        companies.insert("data", carr);

        auto out = Catalog::parseNetworks(companies, types);
        vtest::checkEq(out.size(), 2, "two networks kept, studio+untyped dropped");
        vtest::checkEq(static_cast<int>(out.at(0).kind), static_cast<int>(SubjectKind::Network), "network kind");
        vtest::checkStr(out.at(0).id, "389", "AMC id");
        vtest::checkStr(out.at(0).name, "AMC", "AMC name");
        vtest::checkStr(out.at(1).name, "HBO", "HBO name");
    }

    // --- parseNetworks: empty catalog yields nothing, no crash ---
    {
        auto out = Catalog::parseNetworks(QJsonObject(), QJsonObject());
        vtest::checkEq(out.size(), 0, "empty types+companies -> no networks");
    }

    return 0;
}
