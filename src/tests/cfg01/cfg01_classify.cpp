// CFG-01 classifier v3 (test plan §4, normative) — QtCore ONLY, no product
// code linked. Parse + structural guard + semantic equality + sha256
// byte-identity. NO find/contains/suffix/last-byte heuristics anywhere in the
// classification path (the v1/v2 substring classifiers misread complete
// writes; their outputs stay archived as historical audit artifacts).
//
// Modes:
//   cfg01-classify selftest <evdir> <golden-dir>
//       GV1–GV7 (§4.3). All must classify exactly, else exit 3 (rig-validity).
//       Writes <evdir>/golden-selftest.json. GV2's sha is pinned here at
//       promotion (recorded in the manifest).
//   cfg01-classify batch <evdir>
//       Reads <evdir>/raw-rows.jsonl + per-trial byte snapshots, classifies,
//       writes <evdir>/rows.jsonl + <evdir>/summary.json. Exit 0 on success,
//       1 on rig error. The GATE verdict is applied by run_kill_sweep.py
//       (calibration + variant expectations live there).

#include "cfg01_fixtures.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>
#include <cstdio>

namespace {

QByteArray sha(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

QByteArray readBytes(const QString &p, bool *exists)
{
    QFile f(p);
    *exists = f.exists();
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// §4.1 algorithm, verbatim semantics.
QString classify(const QByteArray &bytes, bool exists, const QByteArray &preSha,
                 const QJsonObject &intended)
{
    if (!exists)                          return QStringLiteral("MISSING");
    if (bytes.isEmpty())                  return QStringLiteral("ZEROED");
    if (sha(bytes) == preSha)             return QStringLiteral("INTACT-PREWRITE");
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError)
        return QStringLiteral("TRUNCATED");
    if (!doc.isObject() || !cfg01::passesGuard(doc.object()))
        return QStringLiteral("TRUNCATED");
    if (doc.object() == intended)         return QStringLiteral("COMPLETE-POSTWRITE");
    return QStringLiteral("TRUNCATED");   // parseable-but-wrong is still defect
}

struct Rig { QString evdir, golden; };

int selftest(const QString &evdir, const QString &golden)
{
    const QByteArray preSha = cfg01::kF2Sha;
    const QJsonObject intended = cfg01::intendedObject();
    QJsonObject report;
    int failed = 0;

    auto expect = [&](const char *gv, const QByteArray &bytes, bool exists,
                      const QString &want) {
        const QString got = classify(bytes, exists, preSha, intended);
        const bool ok = got == want;
        if (!ok) ++failed;
        printf("GV %s: want=%s got=%s %s\n", gv, qPrintable(want), qPrintable(got),
               ok ? "PASS" : "FAIL");
        QJsonObject row{{"gv", gv}, {"want", want}, {"got", got},
                        {"sha256", QString::fromLatin1(sha(bytes))},
                        {"size", (qint64)bytes.size()}, {"pass", ok}};
        report[QString::fromLatin1(gv)] = row;
    };

    bool ex = false;
    // GV1 — durable template (the exact file v1 misread as TRUNCATED).
    const QByteArray gv1 = readBytes(golden + "/gv1-template.json", &ex);
    if (!ex || gv1.size() != cfg01::kF2Size || sha(gv1) != cfg01::kF2Sha) {
        printf("RIG-VALIDITY: GV1 durable template missing/mismatched\n");
        return 3;
    }
    expect("GV1", gv1, true, "INTACT-PREWRITE");
    // GV2 — intended post-save doc from the shared fixture header.
    const QByteArray gv2 = cfg01::intendedBytes();
    expect("GV2", gv2, true, "COMPLETE-POSTWRITE");
    // GV3 — durable real mid-string RLIMIT cut (1024 B, invalid JSON).
    const QByteArray gv3 = readBytes(golden + "/gv3-rlimit-cut.json", &ex);
    if (!ex || gv3.size() != 1024 || sha(gv3) != cfg01::kGv3Sha) {
        printf("RIG-VALIDITY: GV3 durable cut missing/mismatched\n");
        return 3;
    }
    expect("GV3", gv3, true, "TRUNCATED");
    // GV4 — zero-byte.
    expect("GV4", QByteArray(), true, "ZEROED");
    // GV5 — absent.
    expect("GV5", QByteArray(), false, "MISSING");
    // GV6 — template with one pad value altered (parseable-but-wrong branch).
    QByteArray gv6 = cfg01::bigFixtureBytes();
    gv6.replace("\"v0007\"", "\"v000X\"");
    expect("GV6", gv6, true, "TRUNCATED");
    // GV7 — F1-CUT mid-token prefix (467 B).
    const QByteArray gv7 = cfg01::fixtureCutBytes();
    if (gv7.size() != cfg01::kF1CutSize || sha(gv7) != cfg01::kF1CutSha) {
        printf("RIG-VALIDITY: F1-CUT pin mismatch (size=%lld sha=%s)\n",
               (long long)gv7.size(), sha(gv7).constData());
        return 3;
    }
    expect("GV7", gv7, true, "TRUNCATED");

    // Fixture-generator identity: the shared header must reproduce the durable
    // template byte-for-byte (else the whole sweep measures the wrong file).
    const QByteArray gen = cfg01::bigFixtureBytes();
    const bool genOk = gen == gv1;
    printf("FIXTURE-IDENTITY generated==durable: %s\n", genOk ? "PASS" : "FAIL");
    if (!genOk) ++failed;
    report["fixture_identity"] = genOk;
    report["gv2_sha256"] = QString::fromLatin1(sha(gv2));

    QFile out(evdir + "/golden-selftest.json");
    out.open(QIODevice::WriteOnly);
    out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    out.close();
    printf("CLASSIFY-SELFTEST: %s\n", failed == 0 ? "PASS" : "RIG-VALIDITY FAIL");
    return failed == 0 ? 0 : 3;
}

int batch(const QString &evdir)
{
    QFile raw(evdir + "/raw-rows.jsonl");
    if (!raw.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "RIG ERROR: no raw-rows.jsonl in %s\n", qPrintable(evdir));
        return 1;
    }
    bool ex = false;
    const QByteArray tmpl = readBytes(evdir + "/template.json", &ex);
    if (!ex || tmpl.size() != cfg01::kF2Size || sha(tmpl) != cfg01::kF2Sha) {
        fprintf(stderr, "RIG ERROR: template.json missing/mismatched\n");
        return 1;
    }
    const QByteArray preSha = sha(tmpl);
    const QJsonObject intended = cfg01::intendedObject();

    QJsonObject counts{{"MISSING", 0}, {"ZEROED", 0}, {"INTACT-PREWRITE", 0},
                       {"TRUNCATED", 0}, {"COMPLETE-POSTWRITE", 0}};
    QFile rows(evdir + "/rows.jsonl");
    rows.open(QIODevice::WriteOnly);
    int total = 0, valid = 0, kills = 0, rlimits = 0;
    while (!raw.atEnd()) {
        const QByteArray line = raw.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonObject row = QJsonDocument::fromJson(line).object();
        const int trial = row.value("trial").toInt();
        const QString kind = row.value("kind").toString();
        const QString snap = QStringLiteral("%1/trials/t%2/config-after.json")
                                 .arg(evdir).arg(trial, 3, 10, QLatin1Char('0'));
        bool exists = false;
        const QByteArray bytes = readBytes(snap, &exists);
        const QString cls = classify(bytes, exists, preSha, intended);
        row["sha256"] = QString::fromLatin1(sha(bytes));
        row["size"] = (qint64)bytes.size();
        row["class"] = cls;
        counts[cls] = counts.value(cls).toInt() + 1;
        ++total;
        if (cls != "MISSING") ++valid;
        if (kind == "kill") ++kills; else if (kind == "rlimit") ++rlimits;
        rows.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        rows.write("\n");
    }
    rows.close();
    raw.close();

    QJsonObject summary{{"counts", counts}, {"total", total}, {"valid", valid},
                        {"kills", kills}, {"rlimits", rlimits},
                        {"template_sha256", QString::fromLatin1(preSha)},
                        {"intended_sha256", QString::fromLatin1(sha(cfg01::intendedBytes()))}};
    QFile out(evdir + "/summary.json");
    out.open(QIODevice::WriteOnly);
    out.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    out.close();
    printf("SUMMARY total=%d valid=%d kills=%d rlimits=%d | ZEROED=%d TRUNCATED=%d "
           "INTACT=%d COMPLETE=%d MISSING=%d\n",
           total, valid, kills, rlimits,
           counts.value("ZEROED").toInt(), counts.value("TRUNCATED").toInt(),
           counts.value("INTACT-PREWRITE").toInt(),
           counts.value("COMPLETE-POSTWRITE").toInt(), counts.value("MISSING").toInt());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cfg01-classify selftest <evdir> <golden-dir> | batch <evdir>\n");
        return 1;
    }
    const QString mode = argv[1];
    if (mode == "selftest" && argc == 4) return selftest(argv[2], argv[3]);
    if (mode == "batch") return batch(argv[2]);
    fprintf(stderr, "bad mode\n");
    return 1;
}