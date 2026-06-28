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
  in_fn && saw_cas && /expect[[:space:]]*==[[:space:]]*key/ {
    reconciles_duplicate = 1
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
    exit(wait_sleeps && saw_cas && reconciles_duplicate &&
         waits_after_cas && bad == "" ? 0 : 1)
  }
' "$ROOT/src/lj_ctype.c"; then
  printf '%s\n' 'callback/blocking blacklist CAS losers must reconcile duplicate winners and yield via ctype_cbblack_wait_no_l()' >&2
  exit 1
fi
if ! awk '
  /^#define LJ_FFI_RECORD_CALLS 0$/ { defoff = 1 }
  /^#if LJ_FFI_RECORD_CALLS$/ && defoff && !seen_harderr { in_harderr = 1 }
  in_harderr && /#error .*IR_CALLXS.*native-state/ { harderr = 1 }
  in_harderr && /^#endif/ { in_harderr = 0; seen_harderr = 1 }
  /^static int crec_call\(jit_State \*J, RecordFFData \*rd, GCcdata \*cd\)$/ {
    in_fn = 1
  }
  in_fn && /#if !LJ_FFI_RECORD_CALLS/ { guarded = 1 }
  in_fn && guarded && /lj_trace_err\(J, LJ_TRERR_BLACKL\)/ { aborts = 1 }
  in_fn && /IR_CALLXS/ { has_callxs = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(defoff && harderr && guarded && aborts && has_callxs ? 0 : 1) }
' "$ROOT/src/lj_crecord.c"; then
  printf '%s\n' 'FFI C-call recorder must hard-disable traced calls until IR_CALLXS has a native protocol' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_blocking
