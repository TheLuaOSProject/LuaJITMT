#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /^LJLIB_CF\(ffi_blocking\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in ffi.blocking(); use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if ! awk '
  /^#define LJ_FFI_RECORD_CALLS 0$/ { defoff = 1 }
  /^static int crec_call\(jit_State \*J, RecordFFData \*rd, GCcdata \*cd\)$/ {
    in_fn = 1
  }
  in_fn && /#if !LJ_FFI_RECORD_CALLS/ { guarded = 1 }
  in_fn && guarded && /lj_trace_err\(J, LJ_TRERR_BLACKL\)/ { aborts = 1 }
  in_fn && /IR_CALLXS/ { has_callxs = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(defoff && guarded && aborts && has_callxs ? 0 : 1) }
' "$ROOT/src/lj_crecord.c"; then
  printf '%s\n' 'FFI C-call recorder must default to the native-state interpreted path until IR_CALLXS has a native protocol' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_blocking
