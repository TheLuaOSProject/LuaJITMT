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
  /^LJ_FUNCA void lj_tab_wait_no_l\(void\)/ {
    in_fn = 1; body = 0; depth = 0
  }
  in_fn && /lj_thr_sleep_ns\(NULL, 1000000\)/ { found = 1 }
  in_fn { track_braces($0) }
  END { exit found ? 0 : 1 }
' "$SRC"; then
  printf '%s\n' 'lj_tab_wait_no_l() must wait via lj_thr_sleep_ns(NULL, 1000000)' >&2
  exit 1
fi

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
  in_fn && /lj_tab_wait_no_l[[:space:]]*\(/ { found = 1 }
  in_fn && /la_cpu_pause[[:space:]]*\(/ { bad = 1 }
  in_fn { track_braces($0) }
  END { exit found && !bad ? 0 : 1 }
' "$SRC"; then
  printf '%s\n' 'lj_tab_store_wait_no_l() must delegate to lj_tab_wait_no_l(), not spin' >&2
  exit 1
fi

if grep -n 'la_cpu_pause[[:space:]]*(' "$SRC" "$ROOT/src/lj_tab.h" "$ROOT/src/lj_obj.h"; then
  printf '%s\n' 'table retry/snapshot paths must use lj_tab_wait_no_l(), not la_cpu_pause()' >&2
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

check_tab_wait_fn() {
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
    !in_fn && $0 ~ fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && /lj_tab_wait_no_l[[:space:]]*\(/ { saw_helper = 1 }
    in_fn && /la_cpu_pause[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0; bad = 1 }
    in_fn { track_braces($0) }
    END {
      if (bad || !saw_helper)
	exit 1
    }
  ' "$SRC"; then
    printf '%s\n' "$fn must use lj_tab_wait_no_l(), not la_cpu_pause()" >&2
    exit 1
  fi
}

check_tab_wait_fn tab_key_retry_once
check_tab_wait_fn tab_val_forward_retry_once
check_tab_wait_fn lj_tab_newkey
check_tab_wait_fn lj_tab_try_newkey_anchor
check_tab_wait_fn lj_tab_try_newkey_chain
check_tab_wait_fn lj_tab_next

check_store_fn lj_tab_trystoretv_cas
check_store_fn lj_tab_storetv_forjit_array_nogc
check_store_fn lj_tab_storetv_forvm_array
check_store_fn lj_tab_storetv_forjit_hash
check_store_fn lj_tab_storetv_forjit_newref
check_store_fn lj_tab_storetvn_forvm_array

check_store_fn_file() {
  file=$1
  fn=$2
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
    !in_fn &&
    $0 ~ "^[[:space:]]*(static |LUA_API |LUALIB_API |TValue \\*)" &&
    $0 ~ fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0
    }
    in_fn && /lj_tab_store_wait_no_l[[:space:]]*\(/ { saw_helper = 1 }
    in_fn && /la_cpu_pause[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0; bad = 1 }
    in_fn { track_braces($0) }
    END {
      if (bad || !saw_helper)
	exit 1
    }
  ' "$file"; then
    printf '%s\n' "$file:$fn must use lj_tab_store_wait_no_l(), not la_cpu_pause()" >&2
    exit 1
  fi
}

check_store_fn_file "$ROOT/src/lib_base.c" gc_stats_storetv_str
check_store_fn_file "$ROOT/src/lib_base.c" gc_stats_storetv_int
check_store_fn_file "$ROOT/src/lib_base.c" base_storestr_str
check_store_fn_file "$ROOT/src/lib_base.c" base_storetab_str
check_store_fn_file "$ROOT/src/lj_lib.c" lib_storefunc_str
check_store_fn_file "$ROOT/src/lj_lib.c" lib_storetv_key
check_store_fn_file "$ROOT/src/lib_table.c" table_insert_shift_store
check_store_fn_file "$ROOT/src/lib_table.c" table_insert_value_store
check_store_fn_file "$ROOT/src/lib_table.c" table_pack_storeint_str
check_store_fn_file "$ROOT/src/lj_api.c" luaL_newmetatable
check_store_fn_file "$ROOT/src/lj_api.c" lua_settable
check_store_fn_file "$ROOT/src/lj_api.c" lua_setfield
check_store_fn_file "$ROOT/src/lj_api.c" lua_rawset
check_store_fn_file "$ROOT/src/lj_api.c" lua_rawseti
check_store_fn_file "$ROOT/src/lib_jit.c" jit_attach_event_store
check_store_fn_file "$ROOT/src/lib_jit.c" jit_util_storetv_str
check_store_fn_file "$ROOT/src/lib_jit.c" jit_util_storetv_int
check_store_fn_file "$ROOT/src/lib_jit.c" jit_profile_registry_store
check_store_fn_file "$ROOT/src/lib_ffi.c" ffi_typeinfo_storeint
check_store_fn_file "$ROOT/src/lib_ffi.c" ffi_typeinfo_storestr
check_store_fn_file "$ROOT/src/lib_ffi.c" ffi_loaded_store
check_store_fn_file "$ROOT/src/lib_ffi.c" ffi_miscmap_store
check_store_fn_file "$ROOT/src/lib_string.c" string_storetab_str
check_store_fn_file "$ROOT/src/lib_threading.c" threading_storeudata_str
check_store_fn_file "$ROOT/src/lj_debug.c" debug_activelines_storebool
check_store_fn_file "$ROOT/src/lj_ctype.c" ctype_storestr_str
check_store_fn_file "$ROOT/src/lj_meta.c" lj_meta_tsettv_pair
