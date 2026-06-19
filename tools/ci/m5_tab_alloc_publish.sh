#!/bin/sh
# Guard table allocation root publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for needle in \
  'static LJ_AINLINE void tab_init_empty(global_State *g, GCtab *t)' \
  'static LJ_AINLINE void tab_publish_new(global_State *g, GCtab *t)' \
  'GCtab * LJ_FASTCALL lj_tab_new0(lua_State *L)' \
  'lj_mem_newgco_unlinked(L, sizetabcolo(asize))' \
  'lj_mem_newgco_unlinked(L, sizeof(GCtab))' \
  'tab_init_empty(g, t)' \
  'cleararray(array, asize)' \
  'newwhite(g, t)' \
  'lj_gc_linkobj(g, obj2gco(t));  /* CAS-publish table after body init. */'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h"; then
    echo "guardrail: missing table allocation publication marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  '_(lj_tab_new) _(lj_tab_new0)' \
  'call extern lj_tab_new0  // (lua_State *L)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_dispatch.h" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing empty TNEW helper marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_mem_newgco\(L, sizetabcolo\(asize\)\)|lj_mem_newobj\(L, GCtab\)' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: GCtab constructors must initialize unlinked storage before root publication" >&2
  exit 1
fi

if ! awk '
  /static GCtab \*newtab\(lua_State \*L,/ { infn = 1; next }
  infn && /tab_publish_new\(g, t\)/ { published = 1 }
  infn && /newhpart\(L, t, hbits\)/ && !published { bad = 1 }
  infn && /^}/ { exit(bad || !published ? 1 : 0) }
  END { if (bad || !published) exit 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: newtab must publish a valid table body before throwing hash growth" >&2
  exit 1
fi

echo "M5 table allocation publication guard passed"
