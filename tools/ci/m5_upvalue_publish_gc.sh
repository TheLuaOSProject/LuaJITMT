#!/bin/sh
# Run the M5 upvalue publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for file in "$ROOT/src/lib_base.c" "$ROOT/src/lib_table.c" \
    "$ROOT/src/lib_string.c"; do
  if hits=$(grep -nF 'lj_lib_upvalue(L' "$file" || true); [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'library C upvalues must be read with lj_lib_upvalue_load_acq()' >&2
    exit 1
  fi
done

if hits=$(grep -nF 'udataV(&fn->c.upvalue[0])' "$ROOT/src/lib_io.c" || true);
then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'io_file_iter must snapshot the hidden file C upvalue before userdata access' >&2
    exit 1
  fi
fi
if ! grep -Fq 'lj_tv_load_acq(&fileuv, &fn->c.upvalue[0]);' \
    "$ROOT/src/lib_io.c"; then
  printf '%s\n' 'io_file_iter is missing its acquire snapshot of the file C upvalue' >&2
  exit 1
fi
if ! grep -Fq 'lj_tv_load_acq(L->top+i, &fn->c.upvalue[1+i]);' \
    "$ROOT/src/lib_io.c"; then
  printf '%s\n' 'io_file_iter must acquire-snapshot read option C upvalues' >&2
  exit 1
fi

if hits=$(grep -nF 'lj_typename(&fn->c.upvalue' "$ROOT/src/lj_err.c" || true);
then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'lj_err_argtype must snapshot C upvalues before type naming' >&2
    exit 1
  fi
fi
if ! grep -Fq 'lj_tv_load_acq(&tv, &fn->c.upvalue[idx-1]);' \
    "$ROOT/src/lj_err.c"; then
  printf '%s\n' 'lj_err_argtype is missing its acquire snapshot of C upvalues' >&2
  exit 1
fi

if ! awk '
  /^LUA_API const char \*lua_getupvalue\(/ { in_fn = 1 }
  in_fn && /index2adr_read\(L, idx, &snap\)/ { saw_index = 1 }
  in_fn && /lj_tv_load_acq\(L->top, val\)/ { saw_acq = 1 }
  in_fn && /^}/ {
    if (!(saw_index && saw_acq))
      exit 1
    in_fn = 0
  }
' "$ROOT/src/lj_api.c"; then
  printf '%s\n' 'lua_getupvalue() must acquire-snapshot selected upvalue cells' >&2
  exit 1
fi

for fn in lua_isuserdata luaL_checkany; do
  if ! awk -v fn="$fn" '
    $0 ~ ("^(LUA_API int|LUALIB_API void) " fn "\\(") { in_fn = 1; found = 1 }
    in_fn && /index2adr_read\(L, idx, &snap\)/ { saw_index = 1 }
    in_fn && /^}/ { in_fn = 0 }
    END { exit(found && saw_index ? 0 : 1) }
  ' "$ROOT/src/lj_api.c"; then
    printf '%s\n' "$fn must acquire-snapshot pseudo-index C upvalues before type checks" >&2
    exit 1
  fi
done

for fn in lua_rawequal lua_equal lua_lessthan; do
  if ! awk -v fn="$fn" '
    $0 ~ ("^LUA_API int " fn "\\(") { in_fn = 1; found = 1 }
    in_fn && /index2adr_read\(L, idx1, &snap1\)/ { saw_idx1 = 1 }
    in_fn && /index2adr_read\(L, idx2, &snap2\)/ { saw_idx2 = 1 }
    in_fn && /^}/ { in_fn = 0 }
    END { exit(found && saw_idx1 && saw_idx2 ? 0 : 1) }
  ' "$ROOT/src/lj_api.c"; then
    printf '%s\n' "$fn must acquire-snapshot pseudo-index C upvalues before comparison" >&2
    exit 1
  fi
done

if ! awk '
  /^static LJ_AINLINE void index2adr_cupvalue_store_rel\(/ { in_fn = 1 }
  in_fn && /api_trace_flush_mutation\(L\)/ { saw_flush = 1 }
  in_fn && /copyTVrel\(L, o, &snap\)/ { saw_rel = 1 }
  in_fn && /lj_gc_pubobjtv\(L, fn, &snap\)/ { saw_pub = 1 }
  in_fn && /^}/ {
    if (!(saw_flush && saw_rel && saw_pub))
      exit 1
    in_fn = 0
  }
' "$ROOT/src/lj_api.c"; then
  printf '%s\n' 'C-upvalue stores must flush traces and release-publish object values' >&2
  exit 1
fi

if ! awk '
  /^static GCstr \*index2adr_number_tostr\(/ { in_fn = 1 }
  in_fn && /index_iscupvalue\(idx\)/ { saw_cupvalue = 1 }
  in_fn && /index2adr_cupvalue_store_rel\(L, idx, &tv\)/ { saw_store = 1 }
  in_fn && /^}/ {
    if (!(saw_cupvalue && saw_store))
      exit 1
    in_fn = 0
  }
' "$ROOT/src/lj_api.c"; then
  printf '%s\n' 'C-upvalue number-to-string coercion must use the release-published store funnel' >&2
  exit 1
fi

if ! awk '
  /^LUA_API void lua_pushcclosure\(/ { in_fn = 1 }
  in_fn && /copyTVrel\(L, &fn->c.upvalue\[n\], L->top\+n\)/ { saw_rel = 1 }
  in_fn && /lj_gc_pubobjtv\(L, fn, &fn->c.upvalue\[n\]\)/ { saw_pub = 1 }
  in_fn && /^}/ {
    if (!(saw_rel && saw_pub))
      exit 1
    in_fn = 0
  }
' "$ROOT/src/lj_api.c"; then
  printf '%s\n' 'lua_pushcclosure() must release-publish C upvalue object values' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_upvalue_publish_gc
