#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if grep -nF 'ok && ctype_isstruct(ctype_get(cts, cid)->info)' \
  "$ROOT/src/lj_cdata.c"
then
  printf '%s\n' 'cdata pointer auto-deref must trust lj_ctype_ptrstruct_snapshot() outside the parser lock' >&2
  exit 1
fi
if hits=$(grep -nE -- 'la_load16_acq\(&[^)]*->[[:space:]]*sib\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType sibling acquire-loads are forbidden; use ctype_sib_acq()' >&2
  exit 1
fi
hits=$({ grep -nE -- '\b(ct|root|fct|cta|sct|ctf|rt)->[[:space:]]*sib\b' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true; } |
  grep -v 'not copied to the ct->sib' || true)
if [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'direct shared CType sibling access is forbidden; use ctype_sib_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'gcref_acq\([^)]*->[[:space:]]*name\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType name acquire-loads are forbidden; use ctype_nameobj_acq() or ctype_name_acq()' >&2
  exit 1
fi
if hits=$(grep -nE -- 'la_load32_acq\(&[^)]*->[[:space:]]*(info|size)\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size acquire-loads are forbidden; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_typeinfo_snapshot
