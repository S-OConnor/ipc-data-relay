#!/usr/bin/env bash
# Runs the GitLab CI pipeline end to end on this machine, one stage at a time
# in the order of "stages:" in .gitlab-ci.yml, and stops at the first stage
# that fails. Each stage runs through scripts/ci-local.sh (gitlab-ci-local), so
# jobs see the artifacts and images of the stages before them.
#
# Usage: scripts/pipeline.sh [STAGE...]
#   scripts/pipeline.sh                  # every stage
#   scripts/pipeline.sh test docker      # only these stages (earlier ones must have run)
#
# Exit status: 0 if every stage passed, otherwise the failing stage's status.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Top-level "stages:" list of .gitlab-ci.yml.
mapfile -t all_stages < <(awk '
    /^stages:/ { in_stages = 1; next }
    in_stages && /^[[:space:]]*-[[:space:]]*/ { sub(/^[[:space:]]*-[[:space:]]*/, ""); sub(/[[:space:]]*(#.*)?$/, ""); print; next }
    in_stages && /^[^[:space:]#]/ { exit }
' "$ROOT/.gitlab-ci.yml")
[ "${#all_stages[@]}" -gt 0 ] || { echo "pipeline: no stages found in .gitlab-ci.yml" >&2; exit 1; }

if [ "$#" -gt 0 ]; then
    for s in "$@"; do
        printf '%s\n' "${all_stages[@]}" | grep -qxF -- "$s" || { echo "pipeline: unknown stage '$s' (stages: ${all_stages[*]})" >&2; exit 2; }
    done
    stages=("$@")
else
    stages=("${all_stages[@]}")
fi

# Stages that contain at least one job that runs by default in this pipeline.
# (--list-csv prints "name;stage;when;..." after some log lines.)
active_stages="$("$HERE/ci-local.sh" --list-csv 2>/dev/null \
    | awk -F';' '/^name;stage;/ { table = 1; next } table && $3 != "manual" { print $2 }' | sort -u)"

summary=()
report() {
    echo
    echo "==================== pipeline summary ===================="
    printf '  %s\n' "${summary[@]}"
}

for stage in "${stages[@]}"; do
    if ! grep -qxF -- "$stage" <<<"$active_stages"; then
        summary+=("$(printf '%-12s skipped (no jobs)' "$stage")")
        continue
    fi
    echo
    echo "==================== stage: $stage ===================="
    start=$SECONDS
    status=0
    "$HERE/ci-local.sh" --stage "$stage" || status=$?
    elapsed=$((SECONDS - start))
    if [ "$status" -ne 0 ]; then
        summary+=("$(printf '%-12s FAILED (exit %d, %ds)' "$stage" "$status" "$elapsed")")
        report
        echo "pipeline: stage '$stage' failed" >&2
        exit "$status"
    fi
    summary+=("$(printf '%-12s passed (%ds)' "$stage" "$elapsed")")
done

report
echo "pipeline: all stages passed"
