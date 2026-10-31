#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# 240mp-ci-deploy — the deploy dance, run AS ROOT on the Pi by the CI user.
#
# Installed at /usr/local/bin/240mp-ci-deploy; sudoers grants ghdeploy
# exactly this one command (see /etc/sudoers.d/240mp-ci). The GitHub runner
# calls: sudo /usr/local/bin/240mp-ci-deploy /tmp/<tarball>.tar.gz
# The final `systemctl reboot` kills the ssh session — callers must treat a
# dead connection as "rebooting", then verify by re-SSHing (service active
# + build marker present).
#
# Never restart 240mp.service directly: ExecStopPost powers the Pi off; the
# override-clear + VT-stop sequence below is the sanctioned path.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="/opt/240mp"
OWNER="ian"
TARBALL="${1:?Usage: 240mp-ci-deploy <tarball-path-on-pi>}"

[ -f "$TARBALL" ] || { echo "ERROR: no tarball at $TARBALL"; exit 1; }

# Arch guard: a non-ARM binary + 240mp-stop = poweroff boot loop.
ARCH=$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp 2>/dev/null | file - | grep -o 'ARM aarch64' || true)
if [ "$ARCH" != "ARM aarch64" ]; then
    echo "ERROR: tarball binary is not ARM aarch64 — refusing to deploy."
    exit 1
fi

# Clear ExecStopPost BEFORE anything can make 240mp exit...
printf '[Service]\nExecStopPost=\n' > /etc/systemd/system/240mp.service.d/override.conf
systemctl daemon-reload
# Stop 240mp via VT competition with the getty service (never systemctl stop).
systemctl stop 240mp-terminal.service 2>/dev/null || true
systemctl start 240mp-terminal.service
sleep 2

# Extract (FHS tarball → /opt/240mp), re-own, clean up.
tar -xzf "$TARBALL" --strip-components=3 -C "$INSTALL_DIR"
chown -R "${OWNER}:" "$INSTALL_DIR"
rm -f "$TARBALL"

# Restore ExecStopPost.
printf '[Service]\nExecStopPost=+/usr/local/bin/240mp-stop\n' > /etc/systemd/system/240mp.service.d/override.conf
systemctl daemon-reload

# Reboot so the new binary starts (never restart the service directly).
systemctl reboot
