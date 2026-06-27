#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for required in \
  'static void clib_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'static void clib_lds_checkstop(lua_State *L, FILE *fp, uint32_t actions,' \
  'int had_stopreq)' \
  'clib_fresh_stopreq(L, actions, had_stopreq)' \
  'clib_checkstop_fresh(L, actions, had_stopreq)' \
  'clib_lds_checkstop(L, fp, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lj_clib.c"; then
    printf '%s\n' "FFI ld-script native STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done

if hits=$(awk '
  /^static void clib_lds_checkstop\(lua_State \*L, FILE \*fp, uint32_t actions,/ {
    in_fn = 1
  }
  in_fn && /lj_tg_flags_test_acq|TGF_STOPREQ/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fn && /^}/ {
    in_fn = 0
  }
' "$ROOT/src/lj_clib.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'ld-script read checks must use fresh STOPREQ semantics' >&2
  exit 1
fi

if hits=$(awk '
  /^static void clib_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh {
    print FILENAME ":" FNR ":" $0
  }
  in_fresh && /^}/ {
    in_fresh = 0
  }
' "$ROOT/src/lj_clib.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'clib native action checks must use fresh STOPREQ semantics' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_clib_ldscript
