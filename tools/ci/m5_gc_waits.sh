#!/bin/sh
# Guard GC/GC2 retry waits against native busy-spins.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
GC="$ROOT/src/lj_gc.c"
GC2="$ROOT/src/lj_gc2.c"

if hits=$(grep -n 'la_cpu_pause[[:space:]]*(' "$GC" "$GC2" || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'GC and GC2 retry waits must use no-L native sleep helpers, not la_cpu_pause()' >&2
  exit 1
fi

check_helper() {
  file=$1
  helper=$2
  callee=$3
  if ! awk -v helper="$helper" -v callee="$callee" '
    function track_braces(line) {
      opens = gsub(/\{/, "{", line)
      line = $0
      closes = gsub(/\}/, "}", line)
      if (opens)
	body = 1
      depth += opens - closes
      if (body && depth == 0)
	in_fn = 0
    }
    !in_fn && $0 ~ "^[[:space:]]*static void " helper "[[:space:]]*\\(void\\)" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && $0 ~ callee "[[:space:]]*\\(" { found = 1 }
    in_fn { track_braces($0) }
    END { exit found ? 0 : 1 }
  ' "$file"; then
    printf '%s\n' "$file:$helper must call $callee()" >&2
    exit 1
  fi
}

check_function_wait() {
  file=$1
  fn=$2
  wait=$3
  if ! awk -v fn="$fn" -v wait="$wait" '
    function track_braces(line) {
      opens = gsub(/\{/, "{", line)
      line = $0
      closes = gsub(/\}/, "}", line)
      if (opens)
	body = 1
      depth += opens - closes
      if (body && depth == 0)
	in_fn = 0
    }
    !in_fn && $0 ~ fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && $0 ~ wait "[[:space:]]*\\(" { found = 1 }
    in_fn { track_braces($0) }
    END { exit found ? 0 : 1 }
  ' "$file"; then
    printf '%s\n' "$file:$fn must wait via $wait()" >&2
    exit 1
  fi
}

check_helper "$GC" gc_root_wait_no_l lj_thr_sleep_ns
check_helper "$GC" gc_finreg_claim_wait_no_l gc_root_wait_no_l
check_helper "$GC2" gc2_peer_wait_no_l lj_thr_sleep_ns
check_helper "$GC2" gc2_finalizer_wait_no_l gc2_peer_wait_no_l
check_helper "$GC2" gc2_finreg_claim_wait_no_l gc2_peer_wait_no_l

check_function_wait "$GC" gc2_unlink_root_obj gc_root_wait_no_l
check_function_wait "$GC" lj_gc_sweep_gc2_unmarked gc_root_wait_no_l
check_function_wait "$GC" gc_sweep gc_root_wait_no_l
check_function_wait "$GC2" gc2_worker_control_lock gc2_peer_wait_no_l
check_function_wait "$GC2" lj_gc2_finalizer_drain_owned gc2_peer_wait_no_l
check_function_wait "$GC2" gc2_finclaim_publish gc2_peer_wait_no_l
check_function_wait "$GC2" gc2_finreg_cdata_unlink_root gc2_finreg_claim_wait_no_l
check_function_wait "$GC2" gc2_finreg_udata_unlink_root gc2_finreg_claim_wait_no_l
