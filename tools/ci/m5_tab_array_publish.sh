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
  'LJ_STATIC_ASSERT(sizeof(TabArrayHdr) == 16)' \
  'TABARRAY_ACAP_MASK' \
  'TABARRAY_FLAGS_MASK' \
  'TABARRAY_FLAG_RETIRING' \
  'lj_tab_array_hdr_pack_acap' \
  'lj_tab_array_hdr_init' \
  'setmref(hdr->next_gen, NULL)' \
  'lj_tab_array_nextgen_acq' \
  'lj_tab_array_nextgen_rel' \
  'lj_tab_array_hdr_flags_or_rel' \
  'lj_tab_array_hdrw' \
  'lj_tab_array_bytes' \
  'lj_tab_array_is_colocated' \
  'lj_tab_array_mem_acq' \
  'lj_tab_array_hdr_flags_acq' \
  'lj_tab_array_is_retiring' \
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
  'retry_snapshot' \
  'lj_tab_array_is_retiring(t, array)' \
  '(void)lj_tab_array_snapshot_acq(t, &array)' \
  'lj_tab_array_snapshot_acq(kt, &karray)' \
  'uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array)' \
  'MSize asize = lj_tab_array_snapshot_acq(t, &array)' \
  'MSize i, asize = lj_tab_array_snapshot_acq(t, &array)' \
  'asize = lj_tab_array_snapshot_acq(kt, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict_str, &array)' \
  'asize = lj_tab_array_snapshot_acq(dict_mt, &array)' \
  'lj_tab_array_snapshot_acq(t, &record_array)' \
  '(uint32_t)lj_tab_array_snapshot_acq(tb, &array)' \
  '(uint32_t)lj_tab_array_snapshot_acq(tpl, &array)' \
  'asize = (uint32_t)lj_tab_array_snapshot_acq(tpl, &array)' \
  'lj_tv_load_acq(&val, &array[i])' \
  'lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING)'
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

if ! rg -F -q 'lj_tab_array_hdr_flags_acq(ret->array) == TABARRAY_FLAG_RETIRING' \
    "$ROOT/tests/t-tab-array-publish.c"; then
  echo "guardrail: retired table arrays must carry RETIRING header flags" >&2
  exit 1
fi

if ! rg -F -q 'lj_tab_array_nextgen_acq(ret->array) == array' \
    "$ROOT/tests/t-tab-array-publish.c"; then
  echo "guardrail: retired table arrays must point at replacement generation" >&2
  exit 1
fi

if ! rg -F -q 'lj_tab_array_is_retiring(t, ret->array)' \
    "$ROOT/tests/t-tab-array-publish.c"; then
  echo "guardrail: retired table arrays must be detected by integer access helper predicate" >&2
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

if rg -n 'lj_tab_array_hdr_asize_rel|la_store32_rel\(&lj_tab_array_hdrw\([^)]*\)->(asize|acap)' \
    "$ROOT/src"; then
  echo "guardrail: table array header asize must be immutable after publish" >&2
  exit 1
fi

if rg -n 'hdr->acap = acap' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table array capacity must be packed through header helpers" >&2
  exit 1
fi

if awk '
  /static LJ_AINLINE void \*lj_tab_array_mem_acq\(const GCtab \*t\)/ {
    inmem = 1
    snap = bad = 0
    next
  }
  inmem && /lj_tab_array_snapshot_acq\(t, &array\)/ { snap = 1 }
  inmem && /lj_tab_array_acq\(t\)/ { bad = 1 }
  inmem && /^}/ {
    checked = 1
    inmem = 0
  }
  END { exit checked && snap && !bad ? 0 : 1 }
' "$ROOT/src/lj_obj.h"; then
  :
else
  echo "guardrail: table array memory roots must use array snapshots" >&2
  exit 1
fi

if rg -n 'asize = lj_tab_asize_acq\(dict\)|array = lj_tab_array_acq\(dict\)|idx < lj_tab_asize_acq\(dict_|lj_tab_array_acq\(dict_' \
    "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer dictionary array reads must use array snapshots" >&2
  exit 1
fi

if rg -n 'lj_tab_resize\(L, dict, lj_tab_asize_acq\(dict\)' \
    "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer dictionary resize must preserve snapshot array size" >&2
  exit 1
fi

if awk '
  /LJLIB_CF\(table_pack\)/ {
    inpack = 1
    pack_snap = pack_bad = 0
    next
  }
  inpack && /lj_tab_array_snapshot_acq\(t, &array\)/ { pack_snap = 1 }
  inpack && /lj_tab_array_acq\(t\)/ { pack_bad = 1 }
  inpack && /^}/ { pack_checked = 1; inpack = 0 }
  /static GCtab \*bcread_ktab\(LexState \*ls\)/ {
    inbcread = 1
    bcread_snap = bcread_bad = 0
    next
  }
  inbcread && /lj_tab_array_snapshot_acq\(t, &o\)/ { bcread_snap = 1 }
  inbcread && /lj_tab_array_acq\(t\)/ { bcread_bad = 1 }
  inbcread && /^}/ { bcread_checked = 1; inbcread = 0 }
  END {
    exit pack_checked && pack_snap && !pack_bad &&
	 bcread_checked && bcread_snap && !bcread_bad ? 0 : 1
  }
' "$ROOT/src/lib_table.c" "$ROOT/src/lj_bcread.c"; then
  :
else
  echo "guardrail: private table array fills must use snapshots" >&2
  exit 1
fi

if awk '
  /t = lj_tab_new\(sbufL\(sbx\), narray, hsize2hbits\(nhash\)\)/ {
    indecode = 1
    decode_snap = decode_bad = 0
    next
  }
  indecode && /lj_tab_array_snapshot_acq\(t, &array\)/ { decode_snap = 1 }
  indecode && /lj_tab_array_acq\(t\)/ { decode_bad = 1 }
  indecode && /if \(nhash\)/ { decode_checked = 1; indecode = 0 }
  END { exit decode_checked && decode_snap && !decode_bad ? 0 : 1 }
' "$ROOT/src/lj_serialize.c"; then
  :
else
  echo "guardrail: serializer table decode array fill must use snapshots" >&2
  exit 1
fi

if awk '
  /if \(!t\) \{/ { inctor = 1; next }
  inctor && /lj_tab_array_snapshot_acq\(t, &array\)/ { ctor_snap = 1 }
  inctor && /lj_tab_asize_acq\(t\)/ { ctor_bad = 1 }
  inctor && /lj_gc_check\(fs->L\)/ { ctor_checked = 1; inctor = 0 }
  END { exit ctor_checked && ctor_snap && !ctor_bad ? 0 : 1 }
' "$ROOT/src/lj_parse.c"; then
  :
else
  echo "guardrail: parser template array growth checks must use snapshots" >&2
  exit 1
fi

if rg -n 'TValue \*record_array = lj_tab_array_acq\(t\)' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder array-shape decisions must snapshot array bounds" >&2
  exit 1
fi

if awk '
  /static void rec_idx_bump\(jit_State \*J, RecordIndex \*ix\)/ {
    inbump = 1
    snap = bad = 0
    next
  }
  inbump && /lj_tab_array_snapshot_acq\(tb, &array\)/ { snap++ }
  inbump && /lj_tab_array_snapshot_acq\(tpl, &array\)/ { snap++ }
  inbump && /lj_tab_asize_acq\((tb|tpl)\)|lj_tab_array_acq\(tpl\)/ {
    bad = 1
  }
  inbump && /^}/ {
    bump_checked = 1
    inbump = 0
  }
  /static void rec_tsetm\(jit_State \*J, BCReg ra, BCReg rn, int32_t i\)/ {
    intsetm = 1
    tsetm_snap = tsetm_bad = 0
    next
  }
  intsetm && /lj_tab_array_snapshot_acq\(t, &array\)/ { tsetm_snap = 1 }
  intsetm && /lj_tab_asize_acq\(t\)/ { tsetm_bad = 1 }
  intsetm && /^}/ {
    tsetm_checked = 1
    intsetm = 0
  }
  END {
    exit bump_checked && snap >= 3 && !bad &&
	 tsetm_checked && tsetm_snap && !tsetm_bad ? 0 : 1
  }
' "$ROOT/src/lj_record.c"; then
  :
else
  echo "guardrail: recorder table-bump array shape must use snapshots" >&2
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

if ! awk '
  /void lj_tab_resize\(lua_State \*L,/ { inresize = 1 }
  inresize && /lj_tab_array_nextgen_rel\(oldarray, array\)/ { nextgen = NR }
  inresize && /lj_tab_array_hdr_flags_or_rel\(oldarray, TABARRAY_FLAG_RETIRING\)/ { retiring = NR }
  inresize && /^}/ { inresize = 0 }
  END { exit nextgen && retiring && nextgen < retiring ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: resize must publish retired array next_gen before RETIRING" >&2
  exit 1
fi

echo "M5 table array publication tests passed"
