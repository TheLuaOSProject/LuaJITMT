#!/bin/sh
# Guard M7 FFI C library cache miss/fill concurrency.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'uint32_t cache_token' \
  'clib_cache_lock(CLibrary *cl)' \
  'clib_cache_unlock(CLibrary *cl)' \
  'la_cas32(&cl->cache_token' \
  'lj_tab_getstr(cl->cache, name)' \
  'lj_cdata_new_l(L, cts, id, CTSIZE_PTR)' \
  'lj_tv_isnil_acq(tv)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_clib.c" "$ROOT/src/lj_clib.h"; then
    echo "guardrail: missing FFI clib cache marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'LJ_MT|LUAJIT_THREADSAFE|lj_cdata_new\(cts, id, CTSIZE_PTR\)' \
  "$ROOT/src/lj_clib.c" "$ROOT/src/lj_clib.h"; then
  echo "guardrail: clib cache bridge must stay always-on and explicit-L" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-clib-cache.lua" \
  "${LJ_M7_FFI_CLIB_THREADS:-6}" "${LJ_M7_FFI_CLIB_ITERS:-300}"

echo "M7 FFI clib cache guard passed"
