#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# Deploy 240-MP tarball to Raspberry Pi
#
# Usage:
#   bash deploy-pi.sh <tarball-path> [hostname]
#
# Examples:
#   bash deploy-pi.sh /tmp/240-MP-ci-linux-arm64.tar.gz
#   bash deploy-pi.sh /tmp/240-MP-ci-linux-arm64.tar.gz execubot.weedlun.me
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

TARBALL="${1:?Usage: deploy-pi.sh <tarball-path> [hostname]}"
HOST="${2:-execubot.weedlun.me}"
INSTALL_DIR="/opt/240mp"
OWNER="ian"
PASS=$(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//')

# ── Verify tarball ────────────────────────────────────────────────────────────
if [ ! -f "$TARBALL" ]; then
    echo "Error: tarball not found: $TARBALL"
    exit 1
fi

echo "=== Deploying 240-MP to $HOST ==="
echo "Tarball: $TARBALL"

# ── Stop service (if running) — use 240mp-terminal.service to stop 240mp ───────
# 240mp-terminal.service is a getty on tty1. Starting it stops 240mp.service
# because they compete for the same VT. No ExecStopPost, no poweroff.
echo "Stopping 240mp.service..."
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "sudo systemctl stop 240mp-terminal.service 2>/dev/null && \
     sudo systemctl start 240mp-terminal.service && \
     sleep 2 && \
     sudo systemctl stop 240mp.service 2>/dev/null || echo 'Service not running'"

# Wait for service to fully stop
for i in $(seq 1 10); do
    if sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
        "systemctl is-active 240mp.service 2>/dev/null" | grep -q 'active'; then
        echo "Waiting for service to stop... ($i/10)"
        sleep 2
    else
        echo "Service stopped"
        break
    fi
done

# ── Deploy ────────────────────────────────────────────────────────────────────
echo "Extracting tarball..."
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$TARBALL" "$OWNER@$HOST:/tmp/240-MP-deploy.tar.gz"
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "sudo tar -xzf /tmp/240-MP-deploy.tar.gz --strip-components=3 -C $INSTALL_DIR && \
     sudo chown -R $OWNER: $INSTALL_DIR && \
     rm -f /tmp/240-MP-deploy.tar.gz && \
     sudo rm -f /etc/systemd/system/240mp.service.d/override.conf && \
     sudo systemctl daemon-reload && \
     echo 'Deploy complete — restart service manually or reboot'"

echo "=== Deploy complete ==="
