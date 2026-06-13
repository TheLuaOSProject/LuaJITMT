#!/bin/sh
# Guard C-side table value stores use release-publishing helpers.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
local util = require("jit.util")
local linfo = util.funcinfo(function() return 1 end)
assert(linfo.proto ~= nil and linfo.upvalues ~= nil)
local cinfo = util.funcinfo(print)
assert(cinfo.addr ~= nil and cinfo.upvalues ~= nil)
local t = { 1, 2, 3 }
t.name = "table-value-publish"
assert(t[3] == 3 and t.name == "table-value-publish")
'

for needle in \
  'lj_tab_storetv' \
  'lj_tab_storenil' \
  'lj_tab_storebool' \
  'lj_tab_storeint' \
  'lj_tab_storeintptr' \
  'lj_tab_storestr' \
  'lj_tab_storetab' \
  'lj_tab_storethread' \
  'lj_tab_storeproto' \
  'lj_tab_storefunc' \
  'lj_tab_storeudata' \
  'lj_tab_storenilraw' \
  'copyTVrel(L, dst, src)' \
  'copyTVrel(L, o, L->top+1)' \
  'copyTVrel(L, o, --L->top)' \
  'lj_tab_storetv(L, dst, &val)' \
  'lj_tab_storenil(L, dst)' \
  'lj_tab_storetv(L, &array[i], &base[i])' \
  'lj_tab_storetab(J->L, &node[i].val, tpl)' \
  'lj_tab_storetab(J->L, o, tpl)' \
  'lj_tab_storenil(J->L, &node[i].val)' \
  'lj_tab_storenil(J->L, &array[i])' \
  'copyTVrel(L, o, base+2)' \
  'lj_tab_storefunc(L, tv, fn)' \
  'lj_tab_storenil(L, tv)' \
  'lj_tab_storetv(L, tv, &tmp)' \
  'lj_tab_storeint(L, tv, (int32_t)ct->size)' \
  'lj_tab_storenilraw(&array[i])' \
  'lj_tab_storenilraw(&n->val)' \
  'lj_tab_storenilraw(tv)' \
  'lj_tab_storenilraw(&node[i].val)' \
  'const_slot_store(o, fs->nkn)' \
  'const_slot_store(o, fs->nkgc)' \
  'lj_tab_storebool(L, tv, 1)' \
  'lj_tab_storetv(ls->L, o, &tv)' \
  'lj_tab_storetv(ls->L, lj_tab_set(ls->L, t, &key), &tv)' \
  'copyTVrel(sbufL(sbx), o, &tv)' \
  'settabV(sbufL(sbx), &tv, t)' \
  'lj_tab_storeint(L, lj_tab_newkey(L, dict, &tv), (int32_t)(i-1))'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table value publication marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'set[a-zA-Z0-9_]*V\([^;]*lj_tab_set|lj_tab_set[^;]*\)->u64|lj_tab_newkey\([^;]*\)->u64|copyTV\([^;]*lj_tab_set' \
    "$ROOT/src" --glob '!host/*'; then
  echo "guardrail: raw table slot stores must use lj_tab_store* or copyTVrel" >&2
  exit 1
fi

if rg -n 'copyTV\(L, o, L->top\+1\)|copyTV\(L, o, --L->top\)|copyTV\(L, dst, &val\)|setnilV\(dst\)|copyTV\(L, &array\[i\], &base\[i\]\)' \
    "$ROOT/src/lj_api.c" "$ROOT/src/lib_table.c"; then
  echo "guardrail: API/table library direct table slot stores must release-publish" >&2
  exit 1
fi

if rg -n 'settabV\(J->L, &(node\[i\]\.val|array\[i\]), tpl\)|settabV\(J->L, o, tpl\)|setnilV\(&(node\[i\]\.val|array\[i\])\)' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder template table markers must release-publish" >&2
  exit 1
fi

if rg -n 'copyTV\(L, o, base\+2\)|setnilV\(tv\)|setnumV\(tv,|setintV\(tv,' \
    "$ROOT/src/lib_ffi.c" "$ROOT/src/lj_clib.c"; then
  echo "guardrail: FFI/clib table aliases must release-publish" >&2
  exit 1
fi

if rg -n 'setnilV\(&array\[i\]\)|setnilV\(&n->val\)|setnilV\(tv\)|setnilV\(&node\[i\]\.val\)' \
    "$ROOT/src/lj_tab.c" "$ROOT/src/lj_gc.c"; then
  echo "guardrail: shared table clearing must release-publish nil" >&2
  exit 1
fi

if rg -n 'o->u64 = fs->nk|setboolV\(tv, 1\)' "$ROOT/src/lj_parse.c"; then
  echo "guardrail: parser constant table slot markers must release-publish" >&2
  exit 1
fi

if rg -n 'bcread_ktabk\(ls, o, NULL\)|bcread_ktabk\(ls, lj_tab_set\(ls->L, t, &key\), t\)' \
    "$ROOT/src/lj_bcread.c"; then
  echo "guardrail: bytecode template table slots must release-publish" >&2
  exit 1
fi

if rg -n 'copyTV\(sbufL\(sbx\), o, &tv\)|settabV\(sbufL\(sbx\), o, t\)|setstrV\(sbufL\(sbx\), o,|setintV\(o,|setpriV\(o,|setcdataV\(sbufL\(sbx\), o,|setrawlightudV\(o,' \
    "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer decode outputs must release-publish" >&2
  exit 1
fi

echo "M5 table value publication guard passed"
