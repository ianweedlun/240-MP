#pragma once
// CFG-01 gate common: message capture, checks, file helpers, exit taxonomy,
// and the Tier-1 save_setting bool shim (PRE returns void, FIX returns bool —
// the same instrument source compiles against both snapshots).
//
// Exit taxonomy (test plan §8 D-3 / §11):
//   0 = gate PASS (fixed-behavior predicates met)
//   2 = gate FAIL (product behavior violates the gate)
//   1 = RIG ERROR (setup/harness fault — never a product verdict)
//   3 = RIG-VALIDITY failure (golden vectors / ready timeout / calibration)

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>
#include <cstdio>
#include <cstdlib>

namespace cfg01 {

inline QStringList g_msgs;
inline int g_warnings = 0;
inline int g_checks = 0;
inline int g_failed = 0;

inline void msgHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *tag = type == QtWarningMsg ? "WARN"
                    : type == QtCriticalMsg ? "CRIT"
                    : type == QtFatalMsg ? "FATAL" : "DEBUG";
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        ++g_warnings;
    QString line = QStringLiteral("[%1] %2").arg(QLatin1String(tag), msg);
    g_msgs.append(line);
    fprintf(stderr, "%s\n", qPrintable(line));
}

inline void check(const char *label, bool ok)
{
    ++g_checks;
    if (!ok) ++g_failed;
    printf("CHECK %s: %s\n", label, ok ? "PASS" : "FAIL");
    fflush(stdout);
}

inline QByteArray sha(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

inline QByteArray readFileBytes(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

inline bool writeFileBytes(const QString &p, const QByteArray &b)
{
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const qint64 n = f.write(b);
    f.close();
    return n == b.size();
}

inline void dumpFile(const QString &src, const QString &dst)
{
    QFile::remove(dst);
    QFile::copy(src, dst);
}

inline void writeMessages(const QString &path)
{
    QFile ml(path);
    if (ml.open(QIODevice::WriteOnly)) {
        ml.write(g_msgs.join('\n').toUtf8());
        ml.close();
    }
}

inline bool variantEmpty(const QVariant &v)
{
    return !v.isValid() || v.toString().isEmpty();
}

// Data dir layout: <rundir>/data (config lives here), <rundir>/approot (empty,
// hermetic — no modules scanned). Returns the config.json path.
inline QString setupDataDir(const QString &rundir, const QByteArray &liveBytes)
{
    QDir rd(rundir);
    rd.mkpath("data");
    rd.mkpath("approot");
    const QString cfg = rundir + "/data/config.json";
    if (!liveBytes.isEmpty() && !writeFileBytes(cfg, liveBytes)) {
        fprintf(stderr, "RIG ERROR: fixture write failed: %s\n", qPrintable(cfg));
        exit(1);
    }
    if (!liveBytes.isEmpty())
        dumpFile(cfg, rundir + "/config-before.json");
    return cfg;
}

// Tier-1 shim: FIX save_setting returns bool; PRE returns void. The instrument
// asserts the RETURN where the surface has one, and the disk observable always.
template <typename Core>
inline bool saveSettingShim(Core &core, const QString &m, const QString &k, const QVariant &v)
{
    if constexpr (requires { { core.save_setting(m, k, v) } -> std::same_as<bool>; }) {
        return core.save_setting(m, k, v);
    } else {
        core.save_setting(m, k, v);
        return true;   // PRE surface is void — disk observables carry the verdict
    }
}

inline int verdict(const char *gate)
{
    printf("GATE %s: %s (%d checks, %d failed)\n", gate,
           g_failed == 0 ? "PASS" : "FAIL", g_checks, g_failed);
    fflush(stdout);
    return g_failed == 0 ? 0 : 2;
}

} // namespace cfg01