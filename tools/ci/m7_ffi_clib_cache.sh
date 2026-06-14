#!/bin/sh
# Guard M7 FFI C library cache miss/fill concurrency.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
SRC="$ROOT/src/lj_clib.c $ROOT/src/lj_clib.h $ROOT/src/lj_crecord.c $ROOT/src/lj_gc.c $ROOT/src/lj_gc2.c $ROOT/src/lj_udata.c"

for needle in \
  'CLibCacheEntry *cache_head' \
  'lj_clib_cache_get(CLibrary *cl, GCstr *name)' \
  'clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name' \
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

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-clib-cache.lua" \
  "${LJ_M7_FFI_CLIB_THREADS:-6}" "${LJ_M7_FFI_CLIB_ITERS:-300}"

echo "M7 FFI clib cache guard passed"
