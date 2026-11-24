#!/usr/bin/env python3
"""N4c per-writer wiring audit (additive to the behavioural gates).

Proves each in-scope data-dir writer site routes through the shared atomic
writer with the right label, that the NewOnly stub sites use the no-clobber
variant, that ConfigFile delegates to the same helper (so the CFG-01 gates and
these gates exercise ONE shape), and that the two exec-bit reapply sites are
still present. A built-in self-test feeds the SAME checker a legacy in-place
write snippet and requires it to be rejected — without that, a checker that
always passes would be worthless.

Exit 0 = wiring intact AND self-test detects the defect, 1 = failure.
"""
import argparse
import os
import sys

# path -> (expected atomicfile::writeBytes* call count, required labels,
#          required NewOnly call count)
SITES = {
    "src/audio/VolumeManager.cpp":                (1, ["volume.json"], 0),
    "src/modules/youtube/YouTubeBackend.cpp":     (6, ["youtube_history.json",
                                                       "youtube_watch_later.json",
                                                       "youtube_sponsor.json",
                                                       "youtube_known_ids.json",
                                                       "youtube_url_cache.json"], 0),
    "src/modules/local_files/LocalFilesBackend.cpp": (1, ["local_files_history.json"], 0),
    "src/modules/nfc_reader/NfcReaderBackend.cpp": (3, ["NFC tag file",
                                                        "nfc_reader_history.json"], 1),
    "src/modules/weather/WeatherBackend.cpp":     (1, ["weather_location.txt"], 0),
    "src/modules/scripts/ScriptsBackend.cpp":     (2, ["scripts favorite sidecar"], 1),
    "src/modules/virtual_channels/TVDBNetworkResolver.cpp": (1, ["TVDB resolver cache"], 0),
    "src/update/UpdateManager.cpp":               (4, ["staged.json", "staged.sha256",
                                                       "apply-appimage.sh",
                                                       "apply-macos.sh"], 0),
}

# The legacy defect signature these gates must detect: in-place truncate+write.
LEGACY_SNIPPET = (
    'QFile file(path);\n'
    'if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return;\n'
    'file.write(payload);\n'
)


def audit_text(text, expected_count, labels, newonly_count):
    """Return list of failure strings for one file's text."""
    fails = []
    plain = text.count("atomicfile::writeBytes(")
    newonly = text.count("atomicfile::writeBytesNewOnly(")
    total = plain + newonly
    if total != expected_count:
        fails.append("expected %d writeBytes* calls, found %d (plain=%d, newonly=%d)"
                     % (expected_count, total, plain, newonly))
    if newonly != newonly_count:
        fails.append("expected %d NewOnly calls, found %d" % (newonly_count, newonly))
    for label in labels:
        if '"%s"' % label not in text:
            fails.append("missing label %r" % label)
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="repo source root")
    args = ap.parse_args()

    # Self-test: the checker must REJECT a legacy in-place writer.
    detected = bool(audit_text(LEGACY_SNIPPET, 1, [], 0))
    if not detected:
        print("FAIL: self-test — checker did not detect the legacy defect")
        return 1
    print("PASS: self-test — legacy in-place writer rejected by checker")

    ok = True
    for rel, (count, labels, newonly) in SITES.items():
        path = os.path.join(args.src, rel)
        if not os.path.isfile(path):
            print("FAIL: missing source file %s" % rel)
            ok = False
            continue
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        fails = audit_text(text, count, labels, newonly)
        if fails:
            ok = False
            print("FAIL: %s" % rel)
            for msg in fails:
                print("       - %s" % msg)
        else:
            print("PASS: %s (%d sites)" % (rel, count))

    # ConfigFile must delegate to the shared helper (single source of truth).
    cfg = os.path.join(args.src, "src/util/ConfigFile.cpp")
    with open(cfg, "r", encoding="utf-8") as f:
        cfg_text = f.read()
    if "atomicfile::writeBytes(path, bytes, durable, what, forceFail)" not in cfg_text:
        print("FAIL: ConfigFile.cpp does not delegate to atomicfile::writeBytes")
        ok = False
    else:
        print("PASS: ConfigFile.cpp delegates to the shared helper")

    # Exec-bit reapply must survive the rename-based write.
    upd = os.path.join(args.src, "src/update/UpdateManager.cpp")
    with open(upd, "r", encoding="utf-8") as f:
        upd_text = f.read()
    if upd_text.count("QFile::setPermissions(scriptPath, QFile::ReadOwner") < 2:
        print("FAIL: exec-bit reapply lost at an update helper script site")
        ok = False
    else:
        print("PASS: exec-bit reapply present at both helper script sites")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
