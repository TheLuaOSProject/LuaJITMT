#!/bin/sh
# Guard metamethod lookup callers against live slot result pointers.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(awk '
  FILENAME ~ /\/src\/lj_meta\.h$/ { next }
  FILENAME ~ /\/src\/lj_meta\.c$/ && /^cTValue \*lj_meta_cache\(/ { next }
  FILENAME ~ /\/src\/lj_meta\.c$/ && /^cTValue \*lj_meta_lookup\(/ { next }
  /^[[:space:]]*(\/\*|\*|\/\/)/ { next }
  /(^|[^[:alnum:]_])lj_meta_cache[[:space:]]*\(/ ||
  /(^|[^[:alnum:]_])lj_meta_lookup[[:space:]]*\(/ ||
  /(^|[^[:alnum:]_])lj_meta_fast[[:space:]]*\(/ ||
  /(^|[^[:alnum:]_])lj_meta_fastg[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT"/src/*.c "$ROOT"/src/*.h || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'Lua metamethod users must use lj_meta_*tv() snapshot helpers' >&2
  exit 1
fi

if hits=$(awk '
  FILENAME ~ /\/src\/lj_ctype\.h$/ { next }
  FILENAME ~ /\/src\/lj_ctype\.c$/ && /^cTValue \*lj_ctype_meta\(/ { next }
  /^[[:space:]]*(\/\*|\*|\/\/)/ { next }
  /(^|[^[:alnum:]_])lj_ctype_meta[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT"/src/*.c "$ROOT"/src/*.h || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'ctype metamethod users must use lj_ctype_metatv() snapshots' >&2
  exit 1
fi
