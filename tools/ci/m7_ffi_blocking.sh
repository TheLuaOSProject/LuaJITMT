#!/bin/sh
# M7 FFI guard with Lua suite coverage.
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
  /^LJLIB_CF\(ffi_blocking\)/ { in_fn = 1 }
  in_fn && /lj_ctype_info_snapshot[[:space:]]*[(]/ { snapshot++ }
  in_fn && /lj_ctype_info_wait[[:space:]]*[(]/ { wait++ }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(snapshot >= 2 && wait >= 2 ? 0 : 1) }
' "$ROOT/src/lib_ffi.c"; then
  printf '%s\n' 'ffi.blocking() must wait/retry ctype snapshots before pointer/function validation' >&2
  exit 1
fi
if ! awk '
  /^static void ctype_cbblack_wait_no_l[[:space:]]*[(]void[)]/ {
    in_wait = 1
  }
  in_wait && /lj_thr_sleep_ns[[:space:]]*[(][[:space:]]*NULL[[:space:]]*,[[:space:]]*1000000[[:space:]]*[)]/ {
    wait_sleeps = 1
  }
  in_wait && /^}/ { in_wait = 0 }
  /^void lj_ctype_cb_blacklist[[:space:]]*[(]/ {
    in_fn = 1
  }
  in_fn && /ctype_cbblack_slot_cas[[:space:]]*[(]/ {
    saw_cas = 1
  }
  in_fn && saw_cas && /ctype_cbblack_wait_no_l[[:space:]]*[(]/ {
    waits_after_cas = 1
  }
  in_fn && /la_cpu_pause[[:space:]]*[(]/ {
    bad = FNR ":" $0
  }
  in_fn && /^}/ {
    in_fn = 0
  }
  END {
    if (bad != "")
      print bad
    exit(wait_sleeps && saw_cas && waits_after_cas && bad == "" ? 0 : 1)
  }
' "$ROOT/src/lj_ctype.c"; then
  printf '%s\n' 'callback/blocking blacklist CAS losers must yield via ctype_cbblack_wait_no_l() without spinning' >&2
  exit 1
fi
if ! awk '
  /^#define LJ_FFI_RECORD_CALLS 0$/ { defoff = 1 }
  /^#if LJ_FFI_RECORD_CALLS$/ && defoff && !seen_harderr { in_harderr = 1 }
  in_harderr && /#error .*IR_CALLXS.*native-state/ { harderr = 1 }
  in_harderr && /^#endif/ { in_harderr = 0; seen_harderr = 1 }
  END { exit(defoff && harderr ? 0 : 1) }
' "$ROOT/src/lj_crecord.c"; then
  printf '%s\n' 'LJ_FFI_RECORD_CALLS must default off and hard-error until IR_CALLXS has a native protocol' >&2
  exit 1
fi
if ! awk '
  /typedef struct CCallNativeState/ { state = 1 }
  /LJ_FUNC void lj_ccall_native_save[[:space:]]*[(]/ { save = 1 }
  /LJ_FUNC void lj_ccall_native_enter[[:space:]]*[(]/ { enter = 1 }
  /LJ_FUNC uint32_t lj_ccall_native_leave[[:space:]]*[(]/ { leave = 1 }
  /LJ_FUNC void lj_ccall_native_checkstop[[:space:]]*[(]/ { check = 1 }
  END { exit(state && save && enter && leave && check ? 0 : 1) }
' "$ROOT/src/lj_ccall.h"; then
  printf '%s\n' 'FFI C-call native-state helpers must remain exported for the future IR_CALLXS bridge' >&2
  exit 1
fi
if hits=$(grep -nE 'static .*ccall_native_(save|enter|leave|checkstop)' "$ROOT/src/lj_ccall.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'FFI C-call native-state helpers must not collapse back to file-local statics' >&2
  exit 1
fi
if ! awk '
  /^int lj_ccall_func[[:space:]]*[(]/ { in_fn = 1 }
  in_fn && /lj_ccall_native_save[[:space:]]*[(]/ { save = 1 }
  in_fn && /lj_ccall_native_enter[[:space:]]*[(]/ { enter = 1 }
  in_fn && /lj_ccall_native_leave[[:space:]]*[(]/ { leave = 1 }
  in_fn && /lj_ccall_native_checkstop[[:space:]]*[(]/ { check = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(save && enter && leave && check ? 0 : 1) }
' "$ROOT/src/lj_ccall.c"; then
  printf '%s\n' 'interpreted FFI C calls must route through the exported native-state helper ABI' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_blocking
