#!/bin/sh
# Guard metadata table-store retry loops against native busy-spins.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

check_wait_helper() {
  file=$1
  helper=$2
  if ! awk -v helper="$helper" '
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
    $0 ~ "static .*" helper "[[:space:]]*\\(void\\)" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && /lj_thr_sleep_ns\(NULL, 1000000\)/ { found = 1 }
    in_fn { track_braces($0) }
    END { exit found ? 0 : 1 }
  ' "$file"; then
    printf '%s\n' "$file: $helper must wait via lj_thr_sleep_ns(NULL, 1000000)" >&2
    exit 1
  fi
}

check_store_fn() {
  file=$1
  fn=$2
  helper=$3
  if ! awk -v fn="$fn" -v helper="$helper" '
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
    $0 ~ "^static .*" fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && $0 ~ helper "[[:space:]]*\\(" { saw_helper = 1 }
    in_fn && /la_cpu_pause[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0; bad = 1 }
    in_fn { track_braces($0) }
    END {
      if (bad || !saw_helper)
	exit 1
    }
  ' "$file"; then
    printf '%s\n' "$fn metadata table-store retries must use $helper(), not la_cpu_pause()" >&2
    exit 1
  fi
}

check_wait_helper "$ROOT/src/lj_parse.c" parse_keep_wait_no_l
check_wait_helper "$ROOT/src/lj_serialize.c" serialize_dict_wait_no_l
check_wait_helper "$ROOT/src/lj_record.c" rec_template_wait_no_l

check_store_fn "$ROOT/src/lj_parse.c" parse_keep_storebool parse_keep_wait_no_l
check_store_fn "$ROOT/src/lj_serialize.c" serialize_dict_storeint serialize_dict_wait_no_l
check_store_fn "$ROOT/src/lj_record.c" rec_template_mark_nil rec_template_wait_no_l
