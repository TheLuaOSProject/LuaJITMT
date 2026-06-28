#!/bin/sh
# M7 FFI guard with Lua suite coverage.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*(miscmap|metamap|sizemeta)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*(miscmap|metamap|sizemeta)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState metatype/miscmap access is forbidden; use ctype_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_metatype\)/ || /^LJLIB_CF\(ffi_meta___call\)/ || /^LJLIB_CF\(ffi_meta___tostring\)/ || /^static int ffi_pairs\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI metatype library helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^void LJ_FASTCALL recff_cdata_call\(/ { in_fn = 1 }
  /^static TRef crec_arith_int64\(/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lj_crecord.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in recorded cdata call/metatype lookup; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_metatype\)/ {
    in_fn = 1
    saw_noparse = 0
    saw_stable = 0
    saw_snapshot = 0
    saw_wait = 0
    saw_string_parse = 0
  }
  in_fn && /ffi_checkctype_noparse[[:space:]]*[(]/ { saw_noparse = 1 }
  in_fn && /if[[:space:]]*\(!isstr\)/ { saw_stable = 1 }
  in_fn && saw_stable && !saw_string_parse && /lj_ctype_info_snapshot[[:space:]]*[(]/ {
    saw_snapshot = 1
  }
  in_fn && saw_stable && !saw_string_parse && /lj_ctype_info_wait[[:space:]]*[(]/ {
    saw_wait = 1
  }
  in_fn && /ffi_checkctype[[:space:]]*[(][[:space:]]*L,[[:space:]]*cts,[[:space:]]*NULL[[:space:]]*[)]/ {
    if (!(saw_noparse && saw_stable && saw_snapshot && saw_wait))
      print FNR ":" $0
    saw_string_parse = 1
  }
  in_fn && /^}/ {
    if (!(saw_noparse && saw_stable && saw_snapshot && saw_wait &&
	  saw_string_parse))
      print FNR ":ffi_metatype missing stable snapshot/wait before parser string path"
    in_fn = 0
  }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.metatype() ctype-object inputs must wait/retry type snapshots before parser string path' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_metatype
