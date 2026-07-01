#!/usr/bin/env bash
set -euo pipefail

tag=${1:?usage: tools/release/release_notes.sh <tag>}
script_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
root=${LJ_RELEASE_ROOT:-$script_root}
root=$(CDPATH= cd -- "$root" && pwd)

if [[ ! "$tag" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  printf 'release tag must be b<major>.<minor>.<patch>; got %s\n' "$tag" >&2
  exit 2
fi

git -C "$root" fetch --tags --force >/dev/null 2>&1 || true

tag_commit=$(git -C "$root" rev-parse "${tag}^{}")
prev=""
while IFS= read -r candidate; do
  [ "$candidate" != "$tag" ] || continue
  [[ "$candidate" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]] || continue
  candidate_commit=$(git -C "$root" rev-parse "${candidate}^{}")
  [ "$candidate_commit" != "$tag_commit" ] || continue
  prev=$candidate
  break
done <<EOF
$(git -C "$root" tag --list 'b*.*.*' --sort=-version:refname)
EOF

if [ -n "$prev" ]; then
  git -C "$root" log --format='- %s' "${prev}..${tag}"
else
  git -C "$root" log -1 --format='- %s' "$tag"
fi
