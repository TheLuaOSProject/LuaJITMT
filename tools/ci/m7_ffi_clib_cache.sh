#!/bin/sh
# Guard M7 FFI C library cache miss/fill concurrency.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
SRC="$ROOT/src/lj_clib.c $ROOT/src/lj_clib.h $ROOT/src/lj_crecord.c $ROOT/src/lj_gc.c $ROOT/src/lj_gc2.c $ROOT/src/lj_udata.c"
SRC_IMPL="$ROOT/src/lj_clib.c $ROOT/src/lj_gc.c $ROOT/src/lj_gc2.c"

for needle in \
  'CLibCacheEntry *cache_head' \
  'lj_clib_cache_name_acq(const CLibCacheEntry *e)' \
  'lj_clib_cache_name_rel(CLibCacheEntry *e, GCstr *name)' \
  'lj_clib_cache_val_acq(TValue *dst,' \
  'lj_clib_cache_val_rel(lua_State *L, CLibCacheEntry *e,' \
  'lj_clib_cache_get(CLibrary *cl, GCstr *name)' \
  'clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name' \
  'lj_tv_load_acq(&tv, ctv)' \
  'la_casptr((void **)&cl->cache_head' \
  'lj_gc_arena_markmem(G(L), e)' \
  'lj_cdata_new_l(L, cts, id, CTSIZE_PTR)' \
  'lj_gc_barrierroot(L, &e->val)' \
  'gc_mark_clib_cache(global_State *g, CLibrary *cl)' \
  'gc2_traverse_clib_cache(global_State *g, CLibrary *cl)' \
  'lj_clib_unload(g, (CLibrary *)uddata(ud))' \
  'lj_clib_cache_get(cl, name)'
do
  if ! rg -F -q "$needle" $SRC; then
    echo "guardrail: missing FFI clib cache marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'LJ_MT|LUAJIT_THREADSAFE|uint32_t cache_token|clib_cache_lock|clib_cache_unlock|lj_tab_setstr\(L, cl->cache|lj_tab_getstr\(cl->cache|lj_cdata_new\(cts, id, CTSIZE_PTR\)' \
  $SRC; then
  echo "guardrail: clib cache must use the side cache without the old token/table bridge" >&2
  exit 1
fi

if rg -n 'e->name = name|copyTV\(L, &e->val|la_loadptr_acq\(\(void \*const \*\)&e->name\)|lj_tv_load_acq\(&tv, &e->val\)' \
  $SRC_IMPL; then
  echo "guardrail: clib cache payloads must use the shared acquire/release helpers" >&2
  exit 1
fi

if ! awk '
  /void LJ_FASTCALL recff_clib_index/ { infn = 1; seen = 1 }
  infn && /lj_tv_load_acq\(&tv, ctv\)/ { snap = 1 }
  infn && /tvisnil\(ctv\)|tvisnil\(tv\)|cdataV\(ctv\)|cdataV\(tv\)/ { raw = 1 }
  infn && /^}/ { exit(seen && snap && !raw ? 0 : 1) }
  END { if (!seen) exit 1 }
' "$ROOT/src/lj_crecord.c"; then
  echo "guardrail: recorder clib cache hits must acquire-snapshot cached TValue" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-clib-cache.lua" \
  "${LJ_M7_FFI_CLIB_THREADS:-6}" "${LJ_M7_FFI_CLIB_ITERS:-300}"

"$ROOT/src/luajit" "$ROOT/tests/t-ffi-clib-cache.lua" \
  "${LJ_M7_FFI_CLIB_JIT_THREADS:-2}" "${LJ_M7_FFI_CLIB_JIT_ITERS:-180}"

echo "M7 FFI clib cache guard passed"
