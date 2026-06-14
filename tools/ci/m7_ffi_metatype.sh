#!/bin/sh
# Guard M7 FFI metatype side-map CAS publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'GCRef *metamap' \
  'MSize sizemeta' \
  'ctype_metamap_init_l(lua_State *L, CTState *cts)' \
  'lj_ctype_setmeta(CTState *cts, CTypeID id, GCtab *mt)' \
  'la_cas64(&meta[id].gcptr64, &expect,' \
  'ctype_meta_tab(CTState *cts, CTypeID id)' \
  'lj_gc_barrierroot(L, &tmp);  /* 11.2 metatype side root. */' \
  'lj_gc_arena_markmem(g, cts->metamap)' \
  'lj_gc2_markmem(g, cts->metamap)' \
  'gc_markobj(g, o)' \
  'lj_gc2_markobj(g, o)' \
  'lj_mem_freevec(g, cts->metamap, cts->sizemeta, GCRef)' \
  'lj_err_caller(L, LJ_ERR_PROTMT)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI metatype/miscmap marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /LJLIB_CF\(ffi_metatype\)/ { inmeta = 1 }
  inmeta && /LJLIB_CF\(ffi_gc\)/ { inmeta = 0 }
  inmeta && /lj_ctype_misc_lock\(cts\)|lj_ctype_misc_unlock\(cts\)|lj_tab_setinth\(L, t, -\(int32_t\)rid\)/ { bad = 1 }
  inmeta && /lj_ctype_setmeta\(cts, rid, mt\)/ { sawcas = 1 }
  inmeta && /lj_gc_barrierroot\(L, &tmp\)/ { sawbarrier = 1 }
  END { exit !bad && sawcas && sawbarrier ? 0 : 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.metatype must publish through side-map CAS and root barrier" >&2
  exit 1
fi

if rg -n 'lj_tab_getinth\(cts->miscmap, -|tv = lj_tab_setinth\(L, t, -\(int32_t\)rid\)' \
    "$ROOT/src/lj_ctype.c" "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: metatype lookup/install must not use structural miscmap negative keys" >&2
  exit 1
fi

if ! rg -F -q 'typedef struct { int x; } lj_m7_meta_root_t' \
    "$ROOT/tests/t-ffi-metatype-miscmap.lua"; then
  echo "guardrail: metatype test must verify side-root liveness after GC" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-metatype-miscmap.lua" \
  "${LJ_M7_FFI_META_THREADS:-6}" "${LJ_M7_FFI_META_ITERS:-60}"

echo "M7 FFI metatype/miscmap guard passed"
