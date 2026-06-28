#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! awk '
  /^LJLIB_CF\(buffer_new\)/ { in_fn = 1; found = 1 }
  in_fn && /lj_tv_load_acq\(&optv, opt_dict\)/ { saw_dict = 1 }
  in_fn && /dict_str = tabV\(&optv\)/ { saw_dict_tab = 1 }
  in_fn && /lj_tv_load_acq\(&optv, opt_mt\)/ { saw_mt = 1 }
  in_fn && /dict_mt = tabV\(&optv\)/ { saw_mt_tab = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END {
    exit(found && saw_dict && saw_dict_tab && saw_mt && saw_mt_tab ? 0 : 1)
  }
' "$ROOT/src/lib_buffer.c"; then
  printf '%s\n' 'buffer.new() must acquire-snapshot dict/metatable option slots' >&2
  exit 1
fi

if ! awk '
  /^static void jit_profile_callback\(/ { in_fn = 1; found = 1 }
  in_fn && /lj_tv_load_acq\(&cbtv, tv\)/ { saw_acq = 1 }
  in_fn && /setfuncV\(L2, L2->top\+\+, funcV\(&cbtv\)\)/ { saw_func = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(found && saw_acq && saw_func ? 0 : 1) }
' "$ROOT/src/lib_jit.c"; then
  printf '%s\n' 'jit_profile_callback() must snapshot the hidden callback slot before calling' >&2
  exit 1
fi

"$ROOT/tools/ci/m5_metadata_store_waits.sh"
"$ROOT/tools/ci/m5_tab_store_waits.sh"
"$ROOT/tools/ci/m5_gc_waits.sh"
exec "$ROOT/tools/ci/lua_test.sh" m5_concurrent_objects
