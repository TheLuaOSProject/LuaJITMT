#!/bin/sh
# M7 FFI guard with Lua suite coverage.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*(next|retired_next|retire_epoch)\b' \
  "$ROOT/src/lj_clib.c" \
  "$ROOT/src/lj_gc.c" \
  "$ROOT/src/lj_crecord.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CLibCacheEntry link/epoch access is forbidden; use lj_clib_cache_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static void gc2_traverse_clib_(retired_)?cache\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(next|retired_next|retire_epoch)\b/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CLibCacheEntry link/epoch access is forbidden in GC2 CLibrary cache traversal; use lj_clib_cache_* helpers' >&2
  exit 1
fi
for helper in gc2_clib_cache_retired_acq \
  gc2_clib_cache_retired_store_rlx \
  gc2_clib_cache_retired_cas \
  gc2_clib_cache_retired_xchg_acqrel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 CLibrary cache retired root" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]clib_cache_retired|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]clib_cache_retired' \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 CLibrary cache retired-root access is forbidden; use gc2_clib_cache_retired_* helpers' >&2
  exit 1
fi
if ! grep -q 'lj_clib_cache_reclaim_retired' "$ROOT/src/lj_gc2.c"; then
  printf '%s\n' 'GC2 retired-object drain must reclaim retired CLibrary cache entries' >&2
  exit 1
fi
if ! grep -q 'lj_clib_cache_freeretired' "$ROOT/src/lj_state.c"; then
  printf '%s\n' 'state close must drain retired CLibrary cache entries' >&2
  exit 1
fi
if ! grep -qE '^[[:space:]]*static void clib_cache_publish_wait_no_l[[:space:]]*[(]void[)]' \
    "$ROOT/src/lj_clib.c"; then
  printf '%s\n' 'CLibrary cache publish retries must use clib_cache_publish_wait_no_l()' >&2
  exit 1
fi
if ! awk '
  /^static TValue \*clib_cache_publish[[:space:]]*[(]/ {
    inside = 1
  }
  inside && /clib_cache_publish_wait_no_l[[:space:]]*[(]/ {
    found = 1
  }
  inside && /la_cpu_pause[[:space:]]*[(]/ {
    bad = FILENAME ":" FNR ":" $0
  }
  inside && /^}/ {
    inside = 0
  }
  END {
    if (bad != "")
      print bad
    exit(found && bad == "" ? 0 : 1)
  }
' "$ROOT/src/lj_clib.c"; then
  printf '%s\n' 'clib_cache_publish CAS losers must yield via clib_cache_publish_wait_no_l()' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_clib___index\)/ || /^LJLIB_CF\(ffi_clib___newindex\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI C library extern helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^void LJ_FASTCALL recff_clib_index\(/ { in_fn = 1 }
  /^static TRef crec_toint\(/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lj_crecord.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in recorded FFI C library namespace lookups; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^static CTSize clib_func_argsize\(/ ||
  /^static int clib_getname_wait\(/ ||
  /^TValue \*lj_clib_index\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_clib.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in FFI C library namespace resolution; use ctype_*_acq() helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^TValue \*lj_clib_index\(/ { in_fn = 1 }
  in_fn && /lj_ctype_parse_lock[[:space:]]*[(]/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_clib.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'lj_clib_index must use namespace snapshot wait/retry instead of taking the ctype parser token' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_clib_cache
