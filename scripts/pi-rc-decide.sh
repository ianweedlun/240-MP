#!/usr/bin/env bash
# Pure decide-mode logic for .github/workflows/pi-rc.yml (restore-on-close).
#
# Emits GITHUB_ENV-formatted lines on stdout: PR, PR_NUM, MODE, and (in restore
# mode) REF. A SKIP_REASON line is emitted on skip so the caller can post the
# precise "unknown restore rev" evidence comment.
#
# Args:  <action> <pr-number> <rc-deployed-pr-marker> <restore-rev>
#
# The marker/rev are read over SSH by the CALLER and passed in as plain args.
# That seam is deliberate: the decision logic is fully unit-testable offline
# (scripts/pi-rc-decide-test.sh, registered with CTest) and never depends on
# GitHub runner state.
#
# Issue #91 / PRs #89,#90: the old inline code wrote `PR_NUM=pr-N` to
# $GITHUB_ENV and then compared against `$PR_NUM` in the SAME step, where that
# name was still an unset shell variable (empty). A matching `pr-N` marker
# therefore always read as a mismatch and MODE=skip silently won. Computing
# PR_NUM here as a real shell value makes the match authoritative.
set -e -o pipefail

ACTION="${1:-}"
PR="${2:-}"
RC_PR="${3:-}"
REV="${4:-}"
PR_NUM="pr-$PR"

printf 'PR=%s\n' "$PR"
printf 'PR_NUM=%s\n' "$PR_NUM"

if [ "$ACTION" != 'closed' ]; then
    printf 'MODE=rc\n'
    exit 0
fi

if [ "$RC_PR" != "$PR_NUM" ]; then
    printf 'MODE=skip\n'
    printf 'SKIP_REASON=mismatch\n'
    exit 0
fi

if [ -z "$REV" ]; then
    printf 'MODE=skip\n'
    printf 'SKIP_REASON=unknown-rev\n'
    exit 0
fi

printf 'MODE=restore\n'
printf 'REF=%s\n' "$REV"
