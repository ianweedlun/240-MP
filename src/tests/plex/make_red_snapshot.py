#!/usr/bin/env python3
"""Build a pinned pre-fix PlexBackend tree for the RED half of the
PLEX-JWT-REFRESH-LATCH-01 gate. Only test seams are added: a mutable test URL
and no-op watchdog setter, plus friend declarations so the unchanged regression
suite compiles. No production logic from the fix is copied into this tree."""
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_red_snapshot.py <base-commit> <out-dir>")
        return 2
    base, out = sys.argv[1], pathlib.Path(sys.argv[2])
    repo = subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()

    def show(path: str) -> str:
        return subprocess.check_output(
            ["git", "-C", repo, "show", f"{base}:{path}"], text=True
        )

    (out / "src/modules/plex").mkdir(parents=True, exist_ok=True)
    cpp = show("src/modules/plex/PlexBackend.cpp")
    header = show("src/modules/plex/PlexBackend.h")
    if "static const QString PLEX_TV" not in cpp:
        raise SystemExit("unexpected pre-fix cpp: PLEX_TV literal not found")
    if "private:" not in header:
        raise SystemExit("unexpected pre-fix header: no private section")

    cpp = cpp.replace("static const QString PLEX_TV", "static QString PLEX_TV", 1)
    cpp += (
        "\n// Test-only seam from make_red_snapshot.py (no behavior change).\n"
        "void PlexBackend::setPlexTvUrlForTests(const QString &url) { PLEX_TV = url; }\n"
        "void PlexBackend::setAuthWatchdogMsForTests(int) {}\n"
    )
    header = header.replace(
        "private:",
        "private:\n"
        "    friend class PlexJwtRefreshRegressionTest;\n"
        "    friend struct Rig;\n"
        "public:\n"
        "    static void setPlexTvUrlForTests(const QString &url);\n"
        "    void setAuthWatchdogMsForTests(int ms);\n"
        "private:",
        1,
    )

    (out / "src/modules/plex/PlexBackend.cpp").write_text(cpp)
    (out / "src/modules/plex/PlexBackend.h").write_text(header)
    (out / "src/modules/plex/PlaybackDecision.h").write_text(
        show("src/modules/plex/PlaybackDecision.h")
    )
    util = out / "src/util"
    if util.is_symlink() or util.exists():
        util.unlink()
    util.symlink_to(pathlib.Path(repo) / "src/util", target_is_directory=True)
    print(f"RED snapshot: {out} (base {base})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
