#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# 240mp-ci-deploy — the deploy dance, run AS ROOT on the Pi by the CI user.
#
# Installed at /usr/local/bin/240mp-ci-deploy; sudoers grants ghdeploy
# exactly this one command (see /etc/sudoers.d/240mp-ci). The GitHub runner
# calls: sudo /usr/local/bin/240mp-ci-deploy /tmp/<tarball>.tar.gz
#
# RESTART-SAFE contract (verified on the live Pi 2026-11-12): the unit's
# ExecStop (240mp-mark stamp) + ExecStopPost (240mp-stop) pair keeps the Pi
# powered on system stop/restart/kill and powers off ONLY on a genuine
# machine shutdown (marker-pair discrimination — commits b391736 + b44a30d).
# So v2 uses plain systemctl stop/start: NO reboot, NO override-clear dance,
# NO getty VT competition. The caller's ssh session SURVIVES the call —
# rc=0 now where the old script died mid-reboot (pi-rc.yml tolerates both).
#
# INSTALL_DIR/OWNER are env-overridable for the lab mock harness; the system
# defaults are the Pi paths.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/opt/240mp}"
OWNER="${OWNER:-ian}"
TARBALL="${1:?Usage: 240mp-ci-deploy <tarball-path-on-pi>}"

[ -f "$TARBALL" ] || { echo "ERROR: no tarball at $TARBALL"; exit 1; }
mkdir -p "$INSTALL_DIR"

# Arch guard: a wrong-arch binary must never displace a working one.
ARCH=$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp 2>/dev/null | file - | grep -o 'ARM aarch64' || true)
if [ "$ARCH" != "ARM aarch64" ]; then
    echo "ERROR: tarball binary is not ARM aarch64 — refusing to deploy."
    exit 1
fi

# Stop the app via the unit itself: the restart-safe stop hook keeps the Pi
# powered on a system stop (live-verified 2026-11-12). Inactive is fine.
systemctl stop 240mp.service 2>/dev/null || true

# Extract (FHS tarball → INSTALL_DIR), re-own, clean up.
# QML-only fast path (2026-11-03, #43): if the tarball binary md5 equals the
# installed one, only share/ assets changed — same stop/start, cheaper log.
OLD_MD5=$(md5sum "$INSTALL_DIR/bin/240mp" 2>/dev/null | awk '{print $1}' || echo none)
NEW_MD5=$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp 2>/dev/null | md5sum | awk '{print $1}')
ASSETS_ONLY=0
if [ "$OLD_MD5" = "$NEW_MD5" ] && [ "$OLD_MD5" != "none" ]; then ASSETS_ONLY=1; fi
tar -xzf "$TARBALL" --strip-components=3 -C "$INSTALL_DIR"
chown -R "${OWNER}:" "$INSTALL_DIR"
rm -f "$TARBALL"

# Start the app back: stop-then-start is restart semantics under the
# restart-safe hook, for both the assets-only and the new-binary case —
# no reboot anywhere in v2.
systemctl start 240mp.service
if [ "$ASSETS_ONLY" = "1" ]; then
    echo "ASSETS-ONLY deploy: binary md5 $NEW_MD5 unchanged — stop/start, no reboot."
else
    echo "NEW-BINARY deploy: binary md5 $NEW_MD5 installed — stop/start, no reboot."
fi
exit 0
