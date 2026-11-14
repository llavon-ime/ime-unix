#!/usr/bin/env bash
set -euo pipefail

# Checks that the current commit may be tagged as a release (RELEASING.md,
# "發布前檢查"). It only reads: it fetches origin, and never creates or pushes a
# tag. A pushed tag cannot be withdrawn, so every check here has to pass first.
#
# Usage: scripts/release-preflight.sh [--wait] <version>
#   <version>  the planned version without the v prefix, for example 0.12.3
#   --wait     wait for CI runs that are still in progress instead of failing
#
# Requires git and an authenticated GitHub CLI (gh).

# Workflows that run on every push to main, by their `name:`. A run of each must
# exist for the commit; without this list a commit whose runs have not been
# created yet would pass. Path-filtered workflows are not listed, but any run
# they did start for the commit has to succeed as well.
REQUIRED_WORKFLOWS=(
    "Unix CI"
    "Native macOS CI"
    "Publish fcitx5 AUR package"
)
POLL_SECONDS=60

usage() {
    cat >&2 <<'USAGE'
Usage: scripts/release-preflight.sh [--wait] <version>
  <version>  the planned version without the v prefix, for example 0.12.3
  --wait     wait for CI runs that are still in progress instead of failing
USAGE
    exit 2
}

wait_for_ci=0
version=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wait) wait_for_ci=1 ;;
        -h|--help) usage ;;
        -*) echo "Unknown option: $1" >&2; usage ;;
        *)
            [[ -z "${version}" ]] || usage
            version="$1"
            ;;
    esac
    shift
done
[[ -n "${version}" ]] || usage
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Version must be <major>.<minor>.<patch> without the v prefix: ${version}" >&2
    exit 2
fi
tag="v${version}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

for tool in git gh; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "${tool} is required." >&2
        exit 2
    fi
done

failures=0
pass() { printf '[ok]   %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1"; failures=$((failures + 1)); }

# Without current remote refs the comparisons below would be against stale data.
if ! git fetch --quiet origin main --tags; then
    echo "Could not fetch origin; the checks need the current main and tags." >&2
    exit 1
fi

head_sha="$(git rev-parse HEAD)"
branch="$(git symbolic-ref --quiet --short HEAD || true)"
if [[ "${branch}" == "main" ]]; then
    pass "on main"
else
    fail "on main (currently ${branch:-a detached HEAD})"
fi

# Covers the index, the working tree, untracked files and submodule pointers.
dirty="$(git status --porcelain)"
if [[ -z "${dirty}" ]]; then
    pass "working tree, index and submodules are clean"
else
    fail "working tree, index and submodules are clean"
    printf '%s\n' "${dirty}" | sed 's/^/         /'
fi

if [[ "${head_sha}" == "$(git rev-parse origin/main)" ]]; then
    pass "HEAD is origin/main (${head_sha:0:7})"
else
    fail "HEAD is origin/main (HEAD ${head_sha:0:7}, origin/main $(git rev-parse --short=7 origin/main))"
fi

if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
    fail "${tag} is unused (it already exists locally)"
else
    # ls-remote exits 2 when the ref is absent and with another status when the
    # remote could not be asked.
    remote_status=0
    git ls-remote --exit-code --tags origin "refs/tags/${tag}" >/dev/null 2>&1 || remote_status=$?
    case "${remote_status}" in
        0) fail "${tag} is unused (it already exists on origin)" ;;
        2) pass "${tag} is unused" ;;
        *) fail "${tag} is unused (could not ask origin)" ;;
    esac
fi

previous_tag="$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null || true)"
if [[ -z "${previous_tag}" ]]; then
    pass "no earlier release is reachable from HEAD"
else
    commits="$(git rev-list --count "${previous_tag}..HEAD")"
    if [[ "${commits}" -gt 0 ]]; then
        pass "${commits} commit(s) since ${previous_tag}"
    else
        fail "there are commits since ${previous_tag} (HEAD is already released as ${previous_tag})"
    fi
    newest="$(printf '%s\n%s\n' "${previous_tag#v}" "${version}" | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1)"
    if [[ "${version}" != "${previous_tag#v}" && "${newest}" == "${version}" ]]; then
        pass "${tag} is newer than ${previous_tag}"
    else
        fail "${tag} is newer than ${previous_tag}"
    fi
fi

# One line per workflow that ran for this push: name, status, conclusion, URL.
# A workflow that was re-triggered counts by its newest run.
ci_runs() {
    gh run list --commit "${head_sha}" --event push --branch main --limit 100 \
        --json workflowName,status,conclusion,createdAt,url \
        --jq 'group_by(.workflowName) | map(max_by(.createdAt)) | .[]
              | [.workflowName, .status,
                 (if (.conclusion // "") == "" then "-" else .conclusion end), .url]
              | @tsv'
}

runs=""
reported_pending=""
while :; do
    if ! runs="$(ci_runs)"; then
        echo "Could not read the workflow runs; check 'gh auth status'." >&2
        exit 1
    fi
    pending="$(printf '%s\n' "${runs}" | awk -F '\t' 'NF && $2 != "completed" { print $1 }')"
    for workflow in "${REQUIRED_WORKFLOWS[@]}"; do
        if ! printf '%s\n' "${runs}" | awk -F '\t' -v name="${workflow}" '$1 == name { found = 1 } END { exit !found }'; then
            pending="${pending:+${pending}
}${workflow}"
        fi
    done
    # Waiting only helps when CI is the last open question.
    [[ "${wait_for_ci}" == 1 && "${failures}" == 0 && -n "${pending}" ]] || break
    # Nothing that is still running can make up for a run that already failed.
    if printf '%s\n' "${runs}" | awk -F '\t' '$2 == "completed" && $3 != "success" { failed = 1 } END { exit !failed }'; then
        break
    fi
    if [[ "${pending}" != "${reported_pending}" ]]; then
        printf 'Waiting for: %s\n' "$(printf '%s' "${pending}" | tr '\n' ',' | sed 's/,/, /g')"
        reported_pending="${pending}"
    fi
    sleep "${POLL_SECONDS}"
done

for workflow in "${REQUIRED_WORKFLOWS[@]}"; do
    if ! printf '%s\n' "${runs}" | awk -F '\t' -v name="${workflow}" '$1 == name { found = 1 } END { exit !found }'; then
        fail "CI: ${workflow} has no run for ${head_sha:0:7}"
    fi
done
while IFS=$'\t' read -r workflow status conclusion url; do
    [[ -n "${workflow}" ]] || continue
    if [[ "${status}" != "completed" ]]; then
        fail "CI: ${workflow} is ${status} (${url})"
    elif [[ "${conclusion}" == "success" ]]; then
        pass "CI: ${workflow}"
    else
        fail "CI: ${workflow} ended with ${conclusion} (${url})"
    fi
done <<< "${runs}"

echo
if [[ "${failures}" -gt 0 ]]; then
    echo "${failures} check(s) failed; do not create ${tag}."
    exit 1
fi
echo "${tag} may be created from ${head_sha:0:7}${previous_tag:+ (changes: ${previous_tag}..HEAD)}."
