#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /^(LJ_NORET static void cconv_err_conv_l|static CType \*cconv_childqual|int lj_cconv_compatptr|int lj_cconv_tv_ct_l|int lj_cconv_tv_bf_l|void lj_cconv_bf_tv_l)\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_cconv.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in guarded cconv helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cdata_set_l
