#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /^(LJ_NORET static void cconv_err_conv_l|static CType \*cconv_childqual|int lj_cconv_compatptr|void lj_cconv_ct_ct_l|int lj_cconv_tv_ct_l|int lj_cconv_tv_bf_l|void lj_cconv_bf_tv_l|static void cconv_array_tab_l|static void cconv_substruct_tab_l|static void cconv_struct_tab_l|static void cconv_array_init_l|static void cconv_substruct_init_l|static void cconv_struct_init_l|int lj_cconv_multi_init|void lj_cconv_ct_init_l)\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_cconv.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in guarded cconv helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^CTypeID lj_ccall_ctid_vararg\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccall.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in C-call vararg inference; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^int lj_ccall_func\(/ { in_fn = 1 }
  in_fn && !/CTF_INSERT/ && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccall.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in C-call entry validation; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^(static void ccall_classify_ct|static int ccall_classify_struct\(CTState \*cts, CType \*ct, int \*rcl|static int ccall_struct_arg)\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccall.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in x86_64 POSIX C-call aggregate classification; use ctype_*_acq() helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ccall_set_args\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccall.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in C-call argument setup; use ctype_*_acq() helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cdata_set_l
