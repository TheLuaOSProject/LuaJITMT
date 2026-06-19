#!/bin/sh
# Guard M7 concurrent FFI cdata allocation publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'void lj_gc_linkobj(global_State *g, GCobj *o)' \
  'void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)' \
  'la_cas64(&g->gc.root.gcptr64' \
  'la_cas32(&g->gc.root.gcptr32' \
  'lj_mem_newgco_unlinked(L, sizeof(GCcdata) + sz' \
  'lj_gc_linkobj(g, obj2gco(cd))' \
  'lj_gc_linkobj(g, o);  /* CAS-requeue finalized cdata on root list. */' \
  'lj_gc_linkobj(g, o);  /* CAS-publish closed upvalue on root list. */' \
  'lj_gc_linkobj(g, obj2gco(T));  /* CAS-publish root after body init. */' \
  'lj_cdata_new_(L, CTID_CTYPEID, 4)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI cdata allocation marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'setgcref\(g->gc.root|setgcrefp\(J2G\(J\)->gc.root|lj_obj_setgcwr\(o, g->gc.root|lj_obj_setgcwr\(obj2gco\(T\), J2G\(J\)->gc.root' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_trace.c"; then
  echo "guardrail: runtime root-list publication must use lj_gc_linkobj()" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-alloc.lua" \
  "${LJ_M7_FFI_CDATA_THREADS:-6}" "${LJ_M7_FFI_CDATA_ITERS:-400}"

echo "M7 FFI cdata allocation guard passed"
