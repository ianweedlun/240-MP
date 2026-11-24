#pragma once

// CFG-01: shared config.json persistence utility.
//
// ONE writer + ONE reader for config.json, used by every module backend and
// AppCore. The defect this replaces (torture CFG-01, HIGH/P1): saveConfig
// truncated the LIVE file then did one unchecked write — a power pull mid-write
// (the Pi powers off on exit) left a zero-byte corpse, and loadConfig silently
// returned defaults on ANY parse failure, so the first settings keypress
// persisted defaults OVER the corpse. Permanent, silent, total settings loss.
//
// Write shape (decision D1): QSaveFile + commit, EVERY return value checked
// (open/write/flush/fsync/commit/dir-fsync). Temp lives in the target's own
// directory (QSaveFile guarantees same-filesystem; no EXDEV, no /tmp). File
// fsync BEFORE commit and containing-directory fsync AFTER commit are REQUIRED
// for config.json: atomic rename gives old-or-new, fsync gives durability on
// ext4 delayed allocation + SD-card volatile caches. Durability claim is
// limited to what fsync provides — no stronger power-loss claim. Siblings
// (.bak / quarantine) use the same atomic shape with best-effort durability.
//
// Load policy (decision D2): missing != corrupt.
//   Missing                     -> defaults, Status::Missing, writable.
//   Parses + structural guard   -> Status::Ok.
//   Corrupt + valid .bak        -> quarantine COPY of corrupt bytes, atomic
//                                  restore from bak, Status::RecoveredFromBackup.
//   Corrupt + no valid bak      -> quarantine COPY, live corrupt file LEFT IN
//                                  PLACE byte-identical (it is the evidence and
//                                  the restart re-detection marker), in-memory
//                                  defaults for rendering only, Status::Corrupt
//                                  -> degraded: EVERY save refuses (intrinsic
//                                  writer guard) until an explicit UI-confirmed
//                                  reset. An atomic writer ALONE still wipes
//                                  pre-existing corruption (the report's P2
//                                  case) — the guard is why it lives in here.
//
// Structural guard (minimal, decided): object root; at least one of app /
// modules present; each present one an object. {} is corrupt (the W5 stale-{}
// incident shape). Unknown extra top-level keys are accepted and preserved.
//
// On-disk format unchanged: QJsonDocument::Indented, same keys, no migration.
// Permissions unchanged (QSaveFile preserves the target's mode).

#include <QJsonObject>
#include <QString>

namespace cfgfile {

enum class Status { Ok, Missing, Corrupt, RecoveredFromBackup };

struct LoadResult {
    QJsonObject doc;
    Status status = Status::Missing;
    QString detail;   // parse error / guard reason / I/O error; empty when Ok/Missing
};

// Canonical first-run defaults — the exact shape AppCore has always synthesized
// for a missing/corrupt file (kept so caller behavior is unchanged).
QJsonObject defaultDocument();

// Structural guard described above. Exposed for tests/audit.
bool passesGuard(const QJsonObject &doc);

// Policy load. MAY quarantine + restore from backup (atomic, idempotent).
// Never throws; every failure path is logged with path + reason.
LoadResult load(const QString &dataRoot);

// Guarded atomic write + bak maintenance. REFUSES (false + warning, live file
// byte-identical) when the live config.json exists and fails parse/guard —
// regardless of caller or doc content. Only reset() writes over a corrupt file.
// On success: live == doc (Indented); if a prior valid live file existed, its
// bytes are preserved byte-identical in config.json.bak (atomic, best-effort:
// bak failure warns but does not block the save). First-ever save: no bak.
bool save(const QString &dataRoot, const QJsonObject &doc);

// The ONLY path that writes over a corrupt live file: quarantine the corrupt
// bytes (copy, same naming/retention as load) then atomically write
// freshDefaults. Returns false — leaving everything untouched — when the live
// file is healthy or missing (healthy-reset guard-rail), or when quarantine or
// the write fails (degraded state persists).
bool reset(const QString &dataRoot, const QJsonObject &freshDefaults);

} // namespace cfgfile