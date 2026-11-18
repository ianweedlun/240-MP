#!/usr/bin/env bash
# Regression tests for scripts/pi-rc-decide.sh (issue #91).
# Plain bash, no Qt/display/network — registered as CTest test "pi-rc-decide".
set -e -o pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HELPER="$HERE/pi-rc-decide.sh"
pass=0
fail=0

check() {
    local name="$1" want="$2" got
    shift 2
    got="$(bash "$HELPER" "$@" | paste -sd';' -)"
    got="${got};"
    if [ "$got" = "$want" ]; then
        printf 'PASS %s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$name" "$want" "$got"
        fail=$((fail + 1))
    fi
}

# 1. The recurrence: closed unmerged, marker matches this PR, rev present.
#    MUST restore — this is exactly what silently skipped on #89 and #90.
check 'closed + matching pr-N marker + rev => restore' \
    'PR=89;PR_NUM=pr-89;MODE=restore;REF=3202128abc;' \
    closed 89 pr-89 3202128abc

# 2. A later PR owns the Pi: mismatch must stay skip (never clobber its RC).
check 'closed + different pr-N marker => skip' \
    'PR=89;PR_NUM=pr-89;MODE=skip;SKIP_REASON=mismatch;' \
    closed 89 pr-90 3202128abc

# 3. Marker matches but no saved rev: skip, with the reason that triggers the
#    "land beta manually" evidence comment.
check 'closed + matching marker + empty rev => skip/unknown-rev' \
    'PR=89;PR_NUM=pr-89;MODE=skip;SKIP_REASON=unknown-rev;' \
    closed 89 pr-89 ''

# 4. No marker at all (PR never RC-deployed) => skip.
check 'closed + no marker => skip' \
    'PR=90;PR_NUM=pr-90;MODE=skip;SKIP_REASON=mismatch;' \
    closed 90 '' deadbeef

# 5. Non-closed events still take the rc branch and keep the PR-head ref.
check 'opened => rc' \
    'PR=89;PR_NUM=pr-89;MODE=rc;' \
    opened 89 '' ''

# 6. The precise old failure mode: a STALE PR_NUM in the environment must be
#    ignored. This pins that the value comes from the argument, not the runner.
got="$(PR_NUM=pr-WRONG bash "$HELPER" closed 89 pr-89 3202128abc | paste -sd';' -);"
if [ "$got" = 'PR=89;PR_NUM=pr-89;MODE=restore;REF=3202128abc;' ]; then
    printf 'PASS stale env PR_NUM is ignored\n'
    pass=$((pass + 1))
else
    printf 'FAIL stale env PR_NUM is ignored\n  got: %s\n' "$got"
    fail=$((fail + 1))
fi

printf 'TOTAL: pass=%d fail=%d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
