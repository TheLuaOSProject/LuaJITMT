#!/bin/sh
# Guard central table-store retry loops against native busy-spins.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT/src/lj_tab.c"

if ! awk '
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
  /^LJ_FUNCA void lj_tab_store_wait_no_l\(void\)/ {
    in_fn = 1; body = 0; depth = 0
  }
  in_fn && /lj_thr_sleep_ns\(NULL, 1000000\)/ { found = 1 }
  in_fn { track_braces($0) }
  END { exit found ? 0 : 1 }
' "$SRC"; then
  printf '%s\n' 'lj_tab_store_wait_no_l() must wait via lj_thr_sleep_ns(NULL, 1000000)' >&2
  exit 1
fi

check_store_fn() {
  fn=$1
  if ! awk -v fn="$fn" '
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
    $0 ~ "^LJ_FUNCA .*" fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && /lj_tab_store_wait_no_l[[:space:]]*\(/ { saw_helper = 1 }
    in_fn && /la_cpu_pause[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0; bad = 1 }
    in_fn { track_braces($0) }
    END {
      if (bad || !saw_helper)
	exit 1
    }
  ' "$SRC"; then
    printf '%s\n' "$fn must use lj_tab_store_wait_no_l(), not la_cpu_pause()" >&2
    exit 1
  fi
}

check_store_fn lj_tab_trystoretv_cas
check_store_fn lj_tab_storetv_forjit_array_nogc
check_store_fn lj_tab_storetv_forvm_array
check_store_fn lj_tab_storetv_forjit_hash
check_store_fn lj_tab_storetv_forjit_newref
check_store_fn lj_tab_storetvn_forvm_array
