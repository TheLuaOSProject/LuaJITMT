#!/bin/sh
# Run the M7 FFI CTState lua_State split guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(awk '
  /typedef struct CTState \{/ { in_cts = 1 }
  in_cts && /lua_State[[:space:]]*\*/ { print FNR ":" $0 }
  in_cts && /^\} CTState;/ { in_cts = 0 }
' "$ROOT/src/lj_ctype.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'CTState must not carry lua_State *L; pass active lua_State explicitly' >&2
  exit 1
fi

if hits=$(grep -RInE -- 'cts[[:space:]]*->[[:space:]]*L([^[:alnum:]_]|$)|parse_L([^[:alnum:]_]|$)' \
    "$ROOT/src" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'shared CTState lua_State bridges are forbidden' >&2
  exit 1
fi

if hits=$(grep -RInE -- 'lj_ctype_new[[:space:]]*\(|lj_ctype_intern[[:space:]]*\(' \
    "$ROOT/src" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'old non-explicit-L ctype allocation/intern APIs are forbidden' >&2
  exit 1
fi

if hits=$(grep -RInE -- 'mref(_acq)?\([^)]*ctype_state|setmref(rel)?\([^)]*ctype_state' \
    "$ROOT"/src/*.c "$ROOT"/src/*.h 2>/dev/null | \
    grep -v '/src/lj_ctype.h:912:' | \
    grep -v '/src/lj_ctype.c:1721:' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw C-side CTState global pointer access is forbidden; use ctype_ctsG() or the init publisher' >&2
  exit 1
fi

if hits=$(grep -nE -- 'ctype_state|DISPATCH_GL\(ctype_state\)' \
    "$ROOT/src/vm_x64.dasc" | grep -v 'lj_ctype_ctsG_acq' || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'raw x64 VM CTState loads are forbidden; call lj_ctype_ctsG_acq()' >&2
    exit 1
  fi
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_no_cts_l
