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

# ── Verify tarball ────────────────────────────────────────────────────────────
if [ ! -f "$TARBALL" ]; then
    echo "Error: tarball not found: $TARBALL"
    exit 1
fi

echo "=== Deploying 240-MP to $HOST ==="
echo "Tarball: $TARBALL"

# ── Stop service (if running) ─────────────────────────────────────────────────
echo "Stopping 240mp.service..."
sshpass -f /dev/stdin ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "sudo systemctl stop 240mp.service 2>/dev/null || echo 'Service not running'" \
    < <(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//')

# Wait for service to fully stop
for i in $(seq 1 10); do
    if sshpass -f /dev/stdin ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
        "systemctl is-active 240mp.service 2>/dev/null" \
        < <(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//') | grep -q 'active'; then
        echo "Waiting for service to stop... ($i/10)"
        sleep 2
    else
        echo "Service stopped"
        break
    fi
done

# ── Deploy ────────────────────────────────────────────────────────────────────
echo "Extracting tarball..."
sshpass -f /dev/stdin ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "sudo tar -xzf /tmp/240-MP-deploy.tar.gz --strip-components=3 -C $INSTALL_DIR && \
     sudo chown -R $OWNER: $INSTALL_DIR && \
     echo 'Deploy complete'" \
    < <(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//')

# ── Start service ─────────────────────────────────────────────────────────────
echo "Starting 240mp.service..."
sshpass -f /dev/stdin ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "sudo systemctl start 240mp.service && \
     sleep 3 && \
     sudo systemctl status 240mp.service --no-pager" \
    < <(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//')

echo "=== Deploy complete ==="
