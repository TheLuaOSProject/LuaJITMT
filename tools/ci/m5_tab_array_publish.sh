#!/bin/sh
# Build and run M5 table array publication/retirement guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-array-publish

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-array-publish.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'lj_tab_array_acq' \
  'lj_tab_array_rel' \
  'lj_tab_asize_acq' \
  'lj_tab_asize_rel' \
  'TabArrayHdr' \
  'TABARRAY_ACAP_MASK' \
  'TABARRAY_FLAGS_MASK' \
  'TABARRAY_FLAG_RETIRING' \
  'lj_tab_array_hdr_pack_acap' \
  'lj_tab_array_hdr_init' \
  'lj_tab_array_hdrw' \
  'lj_tab_array_bytes' \
  'lj_tab_array_is_colocated' \
  'lj_tab_array_mem_acq' \
  'lj_tab_array_hdr_flags_acq' \
  'lj_tab_array_snapshot_acq' \
  'TabArrayRetire' \
  'retired_arrays' \
  'tab_array_new' \
  'lj_tab_array_hdr_init(hdr, asize, acap)' \
  'tab_array_free' \
  'tab_array_retire_reserve' \
  'tab_array_retire_arm' \
  'lj_tab_array_rel(t, array)' \
  'lj_tab_asize_rel(t, asize)' \
  'static LJ_AINLINE cTValue *lj_tab_getint' \
  'static LJ_AINLINE TValue *lj_tab_setint' \
  'lj_tab_array_snapshot_acq(kt, &karray)' \
  'uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array)' \
  'size_t hi = (size_t)lj_tab_array_snapshot_acq(t, &array)' \
  'MSize asize = lj_tab_array_snapshot_acq(t, &array)' \
  'MSize i, asize = lj_tab_array_snapshot_acq(t, &array)' \
  'asize = lj_tab_array_snapshot_acq(kt, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict_str, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict_mt, &array)' \
  'lj_tab_array_snapshot_acq(t, &record_array)' \
  'lj_tv_load_acq(&val, &array[i])'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table array publication marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'lj_tab_array_hdr_flags_acq(oldarray) == 0' \
    "$ROOT/tests/t-tab-array-publish.c"; then
  echo "guardrail: table array test must assert zero header flags" >&2
  exit 1
fi

if rg -n 'tvref\([^[:space:]]*->array\)|setmref\([^,]*->array' \
    "$ROOT/src" --glob '!lj_obj.h' --glob '!host/*' --glob '!vm_*.dasc' \
    --glob '!lj_asm_*.h'; then
  echo "guardrail: table arrays must use lj_tab_array_* helpers in C code" >&2
  exit 1
fi

if rg -n 'lj_mem_realloc(vec)?\([^;]*(array|t->array|oldarray)' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table resize must retire old arrays, not realloc them" >&2
  exit 1
fi

if rg -n '#define (inarray|arrayslot|lj_tab_getint|lj_tab_setint)' \
    "$ROOT/src/lj_tab.h"; then
  echo "guardrail: integer table access must use snapshot inline functions" >&2
  exit 1
fi

if rg -n 'lj_tab_array_hdr_asize_rel|la_store32_rel\(&lj_tab_array_hdrw' \
    "$ROOT/src"; then
  echo "guardrail: table array header asize must be immutable after publish" >&2
  exit 1
fi

if rg -n 'hdr->acap = acap' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table array capacity must be packed through header helpers" >&2
  exit 1
fi

if rg -n 'asize = lj_tab_asize_acq\(dict\)|array = lj_tab_array_acq\(dict\)|idx < lj_tab_asize_acq\(dict_|lj_tab_array_acq\(dict_' \
    "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer dictionary array reads must use array snapshots" >&2
  exit 1
fi

if rg -n 'TValue \*record_array = lj_tab_array_acq\(t\)' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder array-shape decisions must snapshot array bounds" >&2
  exit 1
fi

if awk '
  /static IRType rec_next_types\(GCtab \*t, uint32_t idx\)/ {
    innext = 1
    snap = bad = 0
    next
  }
  innext && /lj_tab_array_snapshot_acq\(t, &array\)/ { snap = 1 }
  innext && /lj_tab_asize_acq\(t\)/ { bad = 1 }
  innext && /lj_tab_array_acq\(t\)/ { bad = 1 }
  innext && /^}/ {
    checked = 1
    innext = 0
  }
  END { exit checked && snap && !bad ? 0 : 1 }
' "$ROOT/src/lj_record.c"; then
  :
else
  echo "guardrail: recorder next() type scan must use array snapshots" >&2
  exit 1
fi

echo "M5 table array publication tests passed"
