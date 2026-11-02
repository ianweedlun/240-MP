#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# Deploy 240-MP tarball to Raspberry Pi — key-auth only (no password secrets).
#
# Usage:
#   bash deploy-pi.sh <tarball-path> [hostname]
#   bash deploy-pi.sh --latest <branch> [hostname]
#
# Examples:
#   bash deploy-pi.sh --latest feature/retroarch-playlist-nav
#   bash deploy-pi.sh --latest feature/retroarch-full-functionality execubot
#   bash deploy-pi.sh /tmp/240-MP-ci-linux-arm64.tar.gz
#
# Auth model (matches .hermes.md "Pi SSH access model"):
#   - Default LAN/agent path: user ian, identity ~/.ssh/id_pi_agent, host 'execubot'.
#   - PI_SSH_IDENTITY GitHub repo secret (if present) is preferred and staged to
#     a temp key file; PI_SSH_KEY env var overrides the local key path.
#   - No PI_SSH_PASSWORD anywhere — this Pi is key-only.
#
# Install runs through the Pi's root wrapper /usr/local/bin/240mp-ci-deploy (the
# same one the CI deploy-pi.yml chain calls). It reboots the Pi; the caller's
# ssh dying mid-command is EXPECTED. We then re-SSH to verify service-active and
# binary md5 against the tarball.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO="ianweedlun/240-MP"
PIUSER="${PI_SSH_USER:-ian}"

# ── Resolve tarball ────────────────────────────────────────────────────────────
if [ "${1:-}" = "--latest" ]; then
    BRANCH="${2:?Usage: deploy-pi.sh --latest <branch> [hostname]}"
    HOST="${3:-execubot}"

    echo "=== Fetching latest CI artifact for branch: $BRANCH ==="
    RUN_JSON=$(gh run list --repo "$REPO" --branch "$BRANCH" --workflow CI --status success --limit 1 --json databaseId,headSha,createdAt)
    if [ -z "$RUN_JSON" ] || [ "$RUN_JSON" = "[]" ]; then
        echo "Error: No successful CI run for branch '$BRANCH' in repo '$REPO'"
        exit 1
    fi
    RUN_ID=$(echo "$RUN_JSON" | jq -r '.[0].databaseId')
    COMMIT=$(echo "$RUN_JSON" | jq -r '.[0].headSha' | cut -c1-8)
    echo "Found CI run $RUN_ID (commit: $COMMIT)"

    TMPDL=$(mktemp -d)
    trap "rm -rf $TMPDL" EXIT

    gh run download "$RUN_ID" --repo "$REPO" -n tarball-linux-arm64 -D "$TMPDL"
    ORIG=$(ls "$TMPDL"/*.tar.gz | head -1)
    TARBALL="$TMPDL/240-MP-${BRANCH//\//-}-linux-arm64-${COMMIT}.tar.gz"
    mv -- "$ORIG" "$TARBALL" 2>/dev/null || [ "$ORIG" = "$TARBALL" ]  # CI may already name it exactly this

    if [ ! -f "$TARBALL" ]; then
        echo "Error: Downloaded artifact not found at $TARBALL"
        ls -la "$TMPDL/"
        exit 1
    fi
    echo "Downloaded: $TARBALL"
elif [ -n "${1:-}" ]; then
    TARBALL="$1"
    HOST="${2:-execubot}"
else
    echo "Usage: deploy-pi.sh <tarball-path> [hostname]"
    echo "       deploy-pi.sh --latest <branch> [hostname]"
    exit 1
fi

if [ ! -f "$TARBALL" ]; then
    echo "Error: tarball not found: $TARBALL"
    exit 1
fi

# ── Arch guard ─────────────────────────────────────────────────────────────────
# The Pi is aarch64. A fat-fingered local build (or wrong CI job) ships an
# x86-64 binary, and 240mp-stop's poweroff-on-exit turns that into a
# boot loop. Verify the arch before we touch the Pi.
if [ "$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp 2>/dev/null | file - | grep -o 'ARM aarch64' || true)" != "ARM aarch64" ]; then
    echo "Error: tarball binary is not ARM aarch64 — refusing to deploy."
    echo "       (A non-ARM binary makes the Pi power off on every boot.)"
    exit 1
fi
LOCAL_MD5=$(tar -xzf "$TARBALL" -O ./usr/local/bin/240mp | md5sum | cut -d' ' -f1)

# ── Identity key ───────────────────────────────────────────────────────────────
# LAN/agent path (user ian): local ~/.ssh/id_pi_agent. For the ghdeploy-style
# path (PI_SSH_USER set to someone else), stage the PI_SSH_IDENTITY repo secret.
KEYFILE="${PI_SSH_KEY:-}"
if [ -z "$KEYFILE" ] && [ "$PIUSER" != "ian" ]; then
    KEY=$(gh secret list --repo "$REPO" --json name,value 2>/dev/null \
          | jq -r '.[] | select(.name=="PI_SSH_IDENTITY") | .value' 2>/dev/null || true)
    if [ -n "$KEY" ] && [ "$KEY" != "null" ]; then
        KEYFILE=$(mktemp)
        printf '%s\n' "$KEY" > "$KEYFILE"
        chmod 600 "$KEYFILE"
    fi
fi
[ -z "$KEYFILE" ] && KEYFILE="$HOME/.ssh/id_pi_agent"
TARGET="$PIUSER@$HOST"

sshrun() { ssh -o ConnectTimeout=15 -i "$KEYFILE" -o StrictHostKeyChecking=accept-new "$TARGET" "$1"; }
scpput() { scp -o ConnectTimeout=15 -i "$KEYFILE" -o StrictHostKeyChecking=accept-new "$1" "$2"; }

echo "=== Deploying to $TARGET ==="
echo "Tarball: $TARBALL"

# ── Upload + root-side deploy (wrapper reboots; dead ssh is expected) ────────
scpput "$TARBALL" "$TARGET:/tmp/240mp-deploy.tar.gz" || { echo "Error: scp upload failed"; exit 1; }
sshrun "sudo -n /usr/local/bin/240mp-ci-deploy /tmp/240mp-deploy.tar.gz" || true  # reboot kills the caller's ssh

# ── Verify over fresh connections: service active + binary md5 match ──────────
sleep 30
for i in $(seq 1 15); do
    OUT=$(sshrun "systemctl is-active 240mp.service; md5sum /opt/240mp/bin/240mp" 2>/dev/null) || { sleep 10; continue; }
    if echo "$OUT" | grep -q "^active" && echo "$OUT" | grep -q "$LOCAL_MD5"; then
        echo "DEPLOYED+VERIFIED: service active, binary md5 matches tarball ($LOCAL_MD5)"
        exit 0
    fi
    echo "verify attempt $i/15: $OUT"
    sleep 10
done
echo "VERIFY FAILED: service active + md5 $LOCAL_MD5 not confirmed after 15 tries"
exit 1
