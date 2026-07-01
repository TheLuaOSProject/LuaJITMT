#!/usr/bin/env bash
set -euo pipefail

tag=${1:?usage: tools/release/release_notes.sh <tag>}
script_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
root=${LJ_RELEASE_ROOT:-$script_root}
root=$(CDPATH= cd -- "$root" && pwd)

release_tag_error() {
  local bad=$1
  if [ "$bad" = "b1.1" ]; then
    printf 'release b1.1 was renamed to b1.0.1; use b1.0.1\n' >&2
  elif [[ "$bad" =~ ^b[0-9]+[.][0-9]+$ ]]; then
    printf 'rolling release tags must be b<major>.<minor>.<patch>; got stale two-component tag %s\n' "$bad" >&2
  else
    printf 'release tag must be b<major>.<minor>.<patch>; got %s\n' "$bad" >&2
  fi
}

history_tag() {
  [ "$1" = "b1.0" ] || [[ "$1" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]]
}

tag_tuple() {
  local body=${1#b}
  local major minor patch
  IFS=. read -r major minor patch <<< "$body"
  patch=${patch:-0}
  printf '%d %d %d\n' "$((10#$major))" "$((10#$minor))" "$((10#$patch))"
}

tag_lt() {
  local a_major a_minor a_patch b_major b_minor b_patch
  read -r a_major a_minor a_patch <<< "$(tag_tuple "$1")"
  read -r b_major b_minor b_patch <<< "$(tag_tuple "$2")"
  if [ "$a_major" -ne "$b_major" ]; then
    [ "$a_major" -lt "$b_major" ]
  elif [ "$a_minor" -ne "$b_minor" ]; then
    [ "$a_minor" -lt "$b_minor" ]
  else
    [ "$a_patch" -lt "$b_patch" ]
  fi
}

if [[ ! "$tag" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  release_tag_error "$tag"
  exit 2
fi

git -C "$root" fetch --tags --force >/dev/null 2>&1 || true

tag_commit=$(git -C "$root" rev-parse "${tag}^{}")
prev=""
while IFS= read -r candidate; do
  [ "$candidate" != "$tag" ] || continue
  history_tag "$candidate" || continue
  tag_lt "$candidate" "$tag" || continue
  candidate_commit=$(git -C "$root" rev-parse "${candidate}^{}")
  [ "$candidate_commit" != "$tag_commit" ] || continue
  prev=$candidate
  break
done <<EOF
$(git -C "$root" tag --list 'b*' --sort=-version:refname)
EOF

if [ -n "$prev" ]; then
  git -C "$root" log --format='- %s' "${prev}..${tag}"
else
  git -C "$root" log -1 --format='- %s' "$tag"
fi
