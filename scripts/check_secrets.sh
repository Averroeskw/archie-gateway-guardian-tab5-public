#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

# Scan tracked files only. Never print matching lines: a failed privacy check
# must not echo the value it is trying to protect.
pattern="(sk-(proj-|ant-)?[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{20,}|BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY|/Users/[^/<[:space:]]+|[A-Za-z0-9-]+\\.[A-Za-z0-9-]+\\.ts\\.net|api[_-]?key[[:space:]]*[:=][[:space:]]*['\"][A-Za-z0-9]|(password|token)[[:space:]]*[:=][[:space:]]*['\"][A-Za-z0-9])"

if git grep -lEI "$pattern" -- . \
    ':(exclude)scripts/check_secrets.sh' \
    ':(exclude)SECURITY.md'; then
    echo "Potential secret or machine-specific value found in tracked file(s)." >&2
    exit 1
fi

if [[ "${1:-}" == "--history" ]]; then
    found=0
    while IFS= read -r commit; do
        files="$(git grep -lEI "$pattern" "$commit" -- . \
            ':(exclude)scripts/check_secrets.sh' \
            ':(exclude)SECURITY.md' 2>/dev/null || true)"
        if [[ -n "$files" ]]; then
            printf 'Potential secret or machine-specific value in commit %s (files redacted).\n' "${commit:0:12}" >&2
            found=1
        fi
    done < <(git rev-list --all)
    if (( found )); then
        exit 1
    fi
    echo "Tracked-file and reachable-history privacy scan passed."
else
    echo "Tracked-file privacy scan passed. Use --history before publishing."
fi
