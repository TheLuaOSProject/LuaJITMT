#!/bin/sh
# Reject retired color-collector entry points in target sources and artifacts.
# src/host/minilua.c is deliberately excluded: minilua is a build-time source
# generator/bootstrap executable and is never linked into a LuaJIT target.

set -eu

script_dir=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -P "$script_dir/../.." && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-gc2-symbols.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

deny='gc_mark gc_mark_claim_white gc_propagate_gray propagatemark gc_sweep gc_sweepstr gc_traverse_tab gc_traverse_func gc_traverse_proto gc_traverse_thread gc_traverse_trace gc_traverse_udata lj_gc_markobj lj_gc_markobj_deep lj_gc_preserveobj lj_gc_mark_trace_slot lj_gc_sweep_gc2_all_arena_bodies lj_gc_resurrect_if_dead'
deny_ere=$(printf '%s\n' "$deny" | sed 's/[[:space:]][[:space:]]*/|/g')
token_ere="(^|[^[:alnum:]_])($deny_ere)([^[:alnum:]_]|$)"
source_hits="$tmpdir/source-hits"
: >"$source_hits"

cd "$root"
for source in $(find src -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.dasc' -o -name '*.S' \) \
    ! -path 'src/host/minilua.c' -print); do
  if LC_ALL=C grep -nHE "$token_ere" "$source" >>"$source_hits"; then
    :
  else
    rc=$?
    if [ "$rc" -ne 1 ]; then
      echo "gc2 retired-symbol gate: cannot scan $source" >&2
      exit 2
    fi
  fi
done

if [ -s "$source_hits" ]; then
  echo "gc2 retired-symbol gate: retired entry point remains in target source:" >&2
  sed 's/^/  /' "$source_hits" >&2
  exit 1
fi

if [ "$#" -eq 0 ]; then
  echo "gc2 retired-symbol gate passed (target sources; host/minilua excluded)"
  exit 0
fi

if [ -n "${NM:-}" ]; then
  nm_tool=$NM
elif command -v llvm-nm >/dev/null 2>&1; then
  nm_tool=$(command -v llvm-nm)
elif command -v nm >/dev/null 2>&1; then
  nm_tool=$(command -v nm)
else
  echo "gc2 retired-symbol gate: neither llvm-nm nor nm is available" >&2
  exit 2
fi

artifact_hits="$tmpdir/artifact-hits"
nm_output="$tmpdir/nm-output"
: >"$artifact_hits"
for artifact in "$@"; do
  if [ ! -f "$artifact" ]; then
    echo "gc2 retired-symbol gate: missing target artifact: $artifact" >&2
    exit 2
  fi
  if ! "$nm_tool" -a "$artifact" >"$nm_output" 2>&1; then
    echo "gc2 retired-symbol gate: $nm_tool cannot inspect $artifact" >&2
    sed 's/^/  /' "$nm_output" >&2
    exit 2
  fi
  awk -v artifact="$artifact" -v deny="$deny" '
    BEGIN {
      n = split(deny, names, " ")
    }
    {
      symbol = $NF
      sub(/^_+/, "", symbol)
      sub(/^imp_+/, "", symbol)
      sub(/^_+/, "", symbol)
      for (i = 1; i <= n; i++) {
        name = names[i]
        suffix = substr(symbol, length(name) + 1, 1)
        if (symbol == name ||
            (substr(symbol, 1, length(name)) == name &&
             (suffix == "." || suffix == "$" || suffix == "@"))) {
          print artifact ": " $0
          break
        }
      }
    }
  ' "$nm_output" >>"$artifact_hits"
done

if [ -s "$artifact_hits" ]; then
  echo "gc2 retired-symbol gate: retired entry point remains in target artifact:" >&2
  sed 's/^/  /' "$artifact_hits" >&2
  exit 1
fi

echo "gc2 retired-symbol gate passed (target artifacts; host/minilua excluded)"
