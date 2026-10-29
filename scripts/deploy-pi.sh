#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# Deploy 240-MP tarball to Raspberry Pi
#
# Usage:
#   bash deploy-pi.sh <tarball-path> [hostname]
#   bash deploy-pi.sh --latest <branch> [hostname]
#
# Examples:
#   bash deploy-pi.sh /tmp/240-MP-ci-linux-arm64.tar.gz
#   bash deploy-pi.sh /tmp/240-MP-ci-linux-arm64.tar.gz execubot.weedlun.me
#   bash deploy-pi.sh --latest feature/retroarch-playlist-nav
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="/opt/240mp"
OWNER="ian"
HOST="${2:-execubot.weedlun.me}"
REPO="ianweedlun/240-MP"

# ── Get Pi password ────────────────────────────────────────────────────────────
# Priority:
# 1. GitHub repo secrets (PI_SSH_PASSWORD) via gh api
# 2. Local fallback ~/hermes/.env
get_pi_password() {
    local pass=""

    # Try GitHub secrets first (works from any machine with gh auth)
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        pass=$(gh secret list --repo "$REPO" --json name,value 2>/dev/null \
            | jq -r '.[] | select(.name=="PI_SSH_PASSWORD") | .value' 2>/dev/null)
        if [ -n "$pass" ] && [ "$pass" != "null" ]; then
            echo "$pass"
            return 0
        fi
    fi

    # Fallback: local .env file
    pass=$(grep -oP 'EXECUBOT_PASSWORD=\K.*' ~/hermes/.env 2>/dev/null | sed 's/^"//;s/"$//')
    if [ -n "$pass" ]; then
        echo "$pass"
        return 0
    fi

    echo "Error: Could not find Pi password. Check GitHub secrets (PI_SSH_PASSWORD) or ~/hermes/.env" >&2
    return 1
}

# ── Parse arguments ────────────────────────────────────────────────────────────
if [ "${1:-}" = "--latest" ]; then
    BRANCH="${2:?Usage: deploy-pi.sh --latest <branch> [hostname]}"
    HOST="${3:-execubot.weedlun.me}"

    echo "=== Fetching latest CI artifact for branch: $BRANCH ==="

    RUN_JSON=$(gh run list --repo "$REPO" --branch "$BRANCH" --event push --status success --json databaseId,headSha,createdAt --limit 1)
    if [ -z "$RUN_JSON" ] || [ "$RUN_JSON" = "[]" ]; then
        echo "Error: No successful CI run found for branch '$BRANCH' in repo '$REPO'"
        echo "Check: gh run list --repo $REPO --branch $BRANCH"
        exit 1
    fi

    RUN_ID=$(echo "$RUN_JSON" | jq -r '.[0].databaseId')
    COMMIT=$(echo "$RUN_JSON" | jq -r '.[0].headSha' | cut -c1-8)

    echo "Found CI run $RUN_ID (commit: $COMMIT)"

    TMPDIR=$(mktemp -d)
    trap "rm -rf $TMPDIR" EXIT

    gh run download "$RUN_ID" --repo "$REPO" -n tarball-linux-arm64 -D "$TMPDIR"
    ORIG=$(ls "$TMPDIR"/*.tar.gz | head -1)
    TARBALL="$TMPDIR/240-MP-${BRANCH//\//-}-linux-arm64-${COMMIT}.tar.gz"
    mv -- "$ORIG" "$TARBALL"

    if [ ! -f "$TARBALL" ]; then
        echo "Error: Downloaded artifact not found at $TARBALL"
        ls -la "$TMPDIR/"
        exit 1
    fi

    echo "Downloaded: $TARBALL"

elif [ -n "${1:-}" ]; then
    TARBALL="$1"
else
    echo "Usage: deploy-pi.sh <tarball-path> [hostname]"
    echo "       deploy-pi.sh --latest <branch> [hostname]"
    exit 1
fi

# ── Get password ───────────────────────────────────────────────────────────────
PASS=$(get_pi_password)

# ── Verify tarball ────────────────────────────────────────────────────────────
if [ ! -f "$TARBALL" ]; then
    echo "Error: tarball not found: $TARBALL"
    exit 1
fi

# The Pi is aarch64. A fat-fingered local build (or wrong CI job) ships an
# x86-64 binary, and 240mp-stop's poweroff-on-exit turns that into a
# boot loop. Verify the arch before we touch the Pi.
TARBALL_ARCH=$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp 2>/dev/null | file - | grep -o 'ARM aarch64' || true)
if [ "$TARBALL_ARCH" != "ARM aarch64" ]; then
    echo "Error: tarball binary is not ARM aarch64 — refusing to deploy."
    echo "       (A non-ARM binary makes the Pi power off on every boot.)"
    exit 1
fi

echo "=== Deploying 240-MP to $HOST ==="
echo "Tarball: $TARBALL"

# ── Stop service (if running) — use 240mp-terminal.service to stop 240mp ───────
# 240mp-terminal.service is a getty on tty1. Starting it stops 240mp.service
# because they compete for the same VT.
# Also clear ExecStopPost to avoid poweroff when 240mp exits due to VT loss.
echo "Stopping 240mp.service via 240mp-terminal.service..."
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$OWNER@$HOST" \
    "echo -e '[Service]\nExecStopPost=' | sudo tee /etc/systemd/system/240mp.service.d/override.conf && \
     sudo systemctl daemon-reload && \
     sudo systemctl stop 240mp-terminal.service 2>/dev/null && \
     sudo systemctl start 240mp-terminal.service && \
     sleep 2"

# Wait for service to fully stop (240mp-terminal takes the VT, forcing 240mp to exit)
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
     echo -e '[Service]\nExecStopPost=+/usr/local/bin/240mp-stop' | sudo tee /etc/systemd/system/240mp.service.d/override.conf && \
     sudo systemctl daemon-reload && \
     sudo systemctl stop 240mp-terminal.service 2>/dev/null; true && \
     echo 'Deploy complete — restart 240mp.service manually or reboot'"

echo "=== Deploy complete ==="