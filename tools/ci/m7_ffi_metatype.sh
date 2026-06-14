#!/bin/sh
# Guard M7 FFI metatype/miscmap mutation bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'uint32_t misc_token' \
  'lj_ctype_misc_lock(CTState *cts)' \
  'la_cas32(&cts->misc_token, &expect, 1, LA_ACQ_REL, LA_ACQ)' \
  'la_futex_wait(&cts->misc_token, 1, 1000000)' \
  'lj_ctype_misc_unlock(CTState *cts)' \
  'lj_ctype_misc_lock(cts)' \
  'lj_ctype_misc_unlock(cts)' \
  'lj_err_caller(L, LJ_ERR_PROTMT)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI metatype/miscmap marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /LJLIB_CF\(ffi_metatype\)/ { inmeta = 1 }
  inmeta && /LJLIB_CF\(ffi_gc\)/ { inmeta = 0 }
  inmeta && /lj_tab_setinth\(L, t, -\(int32_t\)ctype_typeid/ { sawset = 1 }
  inmeta && sawset && /lj_ctype_misc_unlock\(cts\)/ { sawunlock = 1 }
  END { exit sawset && sawunlock ? 1 : 0 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.metatype miscmap store must be covered by misc token" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-metatype-miscmap.lua" \
  "${LJ_M7_FFI_META_THREADS:-6}" "${LJ_M7_FFI_META_ITERS:-60}"

echo "M7 FFI metatype/miscmap guard passed"
