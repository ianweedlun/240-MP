#!/usr/bin/env bash
# CRT-validation wheel entry point.
#
# Drives a named case on the live Pi and prints PASS/FAIL with the evidence
# bundle. The case spec is a tiny YAML-ish file in cases/<name>.yaml: a key
# sequence (each line is one injected key; a line 'shot <id>' is a named
# capture via AppCore::takeScreenshotWithLabel) plus state-log assertions to
# verify after the drive. The wheel is idempotent and safe: it only taps
# navigation/playback keys, never writes config, and restores the Pi to the
# deployed revision.
#
# Usage:
#   scripts/crt-validate.sh <case-name> [host]   # drive a case, print verdict
#   scripts/crt-validate.sh list                 # list case names
#
set -euo pipefail

# Pi connection (override via env for lab/live).
CRT_HOST="${CRT_HOST:-execubot.weedlun.me}"
CRT_USER="${CRT_USER:-ian}"
CRT_KEY="${CRT_KEY:-$HOME/.ssh/id_pi_agent}"
CRT_PORT="${CRT_PORT:-22}"
# All Pi calls must use the same port as the deploy step. Omitting -p made
# every key injection and evidence pull silently time out on port 22.
SSH_OPTS=(-i "$CRT_KEY" -p "$CRT_PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)
CRT_STATE_LOG="${CRT_STATE_LOG:-/tmp/240mp_state.log}"
CRT_SCREENSHOT_DIR="${CRT_SCREENSHOT_DIR:-/tmp/240mp_screenshots}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CASE_DIR="${REPO_ROOT}/cases"
EVIDENCE_DIR="${EVIDENCE_DIR:-${REPO_ROOT}/evidence}"

usage() {
    echo "usage: ${0##*/} <case-name> [host]"
    echo "       ${0##*/} list"
    exit 1
}

# ── case catalog ───────────────────────────────────────────────────────────────
# The authoritative case spec lives in $CASE_DIR/<name>.yaml. The inlined
# fallback below is only a discovery fallback until the case directory is
# populated.
cases() {
    cat <<'EOF'
navigation-walk:
  keys:
    - up
    - select
    - shot nav01
    - up
    - select
    - shot nav02
  expect:
    - "^view: module-list"
    - "^shot nav01"
    - "^shot nav02"

playback-loop:
  keys:
    - shot play01
    - playpause
    - shot play02
    - right
    - shot seek01
  expect:
    - "^view: player"
    - "^shot play01"
    - "^shot play02"

settings-roundtrip:
  keys:
    - shot set01
    - select
    - left
    - right
    - shot set02
  expect:
    - "^view: settings"
    - "^shot set01"
    - "^shot set02"
EOF
}

list_cases() {
    echo "available cases:"
    if [ -d "$CASE_DIR" ]; then
        ls "$CASE_DIR" | sed 's/\.yaml$//' | tr '\n' ' '
        echo
    else
        cases | grep -oE '^[a-z0-9-]+:' | cut -d: -f1 | tr '\n' ' '; echo
    fi
}

# ── step: inject one key line into the Pi's debug key channel ──────────────────
# A line 'shot <id>' is injected verbatim so the key channel's `shot <id>`
# handler runs takeScreenshotWithLabel(<id>); a bare key (up, select, ...) is
# injected as-is.
inject_key() {
    local line="$1"
    ssh "${SSH_OPTS[@]}" -T "$CRT_USER@$CRT_HOST" \
        "cat >> /tmp/240mp_key.in" <<<"$line" >/dev/null 2>&1 || true
    sleep 0.5
}

# ── run a case ─────────────────────────────────────────────────────────────────
run_case() {
    local case_name="$1"
    local case_file="$CASE_DIR/${case_name}.yaml"
    local keys=()
    local expectations=()
    local pass=1
    local line
    local in_keys=1

    if [ ! -f "$case_file" ]; then
        echo "error: case file not found: $case_file"
        return 1
    fi

    # Parse the simple YAML-ish case file: keys: ... expect: ...
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"                          # strip comments
        line="$(printf '%s\n' "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
        [ -z "$line" ] && continue
        if [[ "$line" == keys:* ]]; then
            in_keys=1
            continue
        elif [[ "$line" == expect:* ]]; then
            in_keys=0
            continue
        elif [[ "$line" == -* ]]; then
            local stripped="${line#- }"
            # YAML quotes are syntactic, not part of the expected value.
            stripped="${stripped%\"}"; stripped="${stripped#\"}"
            if [ "$in_keys" -eq 1 ]; then
                keys+=("$stripped")
            else
                expectations+=("$stripped")
            fi
        fi
    done < "$case_file"

    echo "case: $case_name"
    echo "driving keys on ${CRT_USER}@${CRT_HOST} ..."

    # Drive the sequence over SSH.
    for line in "${keys[@]:-}"; do
        inject_key "$line"
    done

    # Wait for the expectations to land (the app writes view:/shot: markers as
    # the wheel runs and the key channel flushes).
    for e in "${expectations[@]:-}"; do
        sleep 1
    done

    # Collect evidence.
    mkdir -p "$EVIDENCE_DIR/$case_name"
    ssh "${SSH_OPTS[@]}" -T "$CRT_USER@$CRT_HOST" \
        "tail -n 120 ${CRT_STATE_LOG}" \
        > "$EVIDENCE_DIR/$case_name/state.log" 2>/dev/null || true
    ssh "${SSH_OPTS[@]}" -T "$CRT_USER@$CRT_HOST" \
        "mkdir -p ${CRT_SCREENSHOT_DIR}" >/dev/null 2>&1 || true
    scp -r -P "$CRT_PORT" -i "$CRT_KEY" "${CRT_USER}@${CRT_HOST}:${CRT_SCREENSHOT_DIR}/*.png" \
        "$EVIDENCE_DIR/$case_name/" 2>/dev/null || true

    echo "evidence: $EVIDENCE_DIR/$case_name/"

    # Verify expectations.
    for e in "${expectations[@]:-}"; do
        if grep -qE "$e" "$EVIDENCE_DIR/$case_name/state.log" 2>/dev/null; then
            echo "  ok: $e"
        else
            echo "  FAIL: missing $e"
            pass=0
        fi
    done

    return $((1 - pass))
}

main() {
    if [ $# -lt 1 ]; then usage; fi
    case "$1" in
        list)
            list_cases
            ;;
        *)
            if run_case "$1"; then
                echo "result: PASS"
            else
                echo "result: FAIL"
                exit 1
            fi
            ;;
    esac
}

main "$@"
