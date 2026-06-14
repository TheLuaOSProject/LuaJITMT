#!/bin/sh
# Guard M7 ffi.blocking recorder blacklist marker.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
SRC="$ROOT/src/lib_ffi.c $ROOT/src/lj_ctype.c $ROOT/src/lj_crecord.c"

for needle in \
  'LJLIB_CF(ffi_blocking)' \
  'ctype_isfunc(ct->info)' \
  'lj_ctype_cb_blacklist(cts, cdata_getptr(cdataptr(cd), sz))' \
  'lj_trace_flushall_hs(L)' \
  'lj_ctype_cb_isblacklisted(cts,' \
  'lj_trace_err(J, LJ_TRERR_BLACKL)'
do
  if ! rg -F -q "$needle" $SRC; then
    echo "guardrail: missing ffi.blocking marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'blocking_token|ffi_blocking_lock|ffi_blocking_unlock|LJ_MT|LUAJIT_THREADSAFE|lj_udata_new\(.*blocking' \
  $SRC; then
  echo "guardrail: ffi.blocking must reuse the pointer blacklist without a token, lock, or wrapper userdata" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" "$ROOT/tests/t-ffi-blocking.lua"

echo "M7 ffi.blocking guard passed"
