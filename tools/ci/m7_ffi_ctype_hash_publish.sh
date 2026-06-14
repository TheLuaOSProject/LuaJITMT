#!/bin/sh
# Guard M7 FFI ctype hash-head publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'uint32_t hash[CTHASH_SIZE]' \
  'ctype_hash_load(CTState *cts, uint32_t h)' \
  'la_load32_acq(&cts->hash[h])' \
  'ctype_hash_cas(CTState *cts, uint32_t h,' \
  'la_cas32(&cts->hash[h], &old, (uint32_t)newid' \
  'ctype_hash_prepend(CTState *cts, uint32_t h, CType *ct, CTypeID id)' \
  'CTypeID id = ctype_hash_load(cts, h)' \
  'ct->next = (CTypeID1)head' \
  'while (!ctype_hash_cas(cts, h, &head, id))' \
  'ctype_hash_prepend(cts, h, &cts->tab[id], id)' \
  'ctype_hash_prepend(cts, h, ct, id)' \
  'CTypeID id = ctype_hash_load(cts, ct_hashname(name))'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype hash publication marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'cts->hash\[[^]]+\]\s*=' "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: ctype hash heads must publish through ctype_hash_prepend" >&2
  exit 1
fi

if rg -n 'ctype_hash_store|la_store32_rel\(&cts->hash' "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: ctype hash publication must stay CAS-prepend" >&2
  exit 1
fi

raw_head_reads='CTypeID[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*='
raw_head_reads="$raw_head_reads[[:space:]]*cts->hash\\["
raw_next_reads='ct->next[[:space:]]*=[[:space:]]*cts->hash\['
if rg -n "($raw_head_reads|$raw_next_reads)" "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: ctype hash heads must read through ctype_hash_load" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-token.lua" \
  "${LJ_M7_FFI_CDEF_THREADS:-6}" "${LJ_M7_FFI_CDEF_ITERS:-120}"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-set-l.lua" \
  "${LJ_M7_FFI_SET_THREADS:-6}" "${LJ_M7_FFI_SET_ITERS:-320}"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-metatype-miscmap.lua" \
  "${LJ_M7_FFI_META_THREADS:-6}" "${LJ_M7_FFI_META_ITERS:-60}"

echo "M7 FFI ctype hash publication guard passed"
