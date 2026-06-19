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
jit.flush()
jit.opt.start("hotloop=1")
local function hot(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 5 do hot(20) end
local traced
for tr = 1, 32 do
  local info = util.traceinfo(tr)
  if info then
    assert(type(info.nins) == "number" and type(info.linktype) == "string")
    traced = tr
    break
  end
end
assert(traced)
local snap
for sn = 0, 32 do
  snap = util.tracesnap(traced, sn)
  if snap then break end
end
assert(snap and type(snap[0]) == "number" and type(snap[1]) == "number")
local lines = debug.getinfo(function()
  local x = 1
  return x
end, "L").activelines
assert(type(lines) == "table")
local saw_line = false
for line, active in pairs(lines) do
  if type(line) == "number" and active == true then
    saw_line = true
    break
  end
end
assert(saw_line)
local t = { 1, 2, 3 }
t.name = "table-value-publish"
assert(t[3] == 3 and t.name == "table-value-publish")
assert(("table-value-publish"):sub(1, 5) == "table")
local function event_cb() end
jit.attach(event_cb, "bc")
jit.attach(event_cb)
do
  local ffi = require("ffi")
  local x = 1LL
  assert(type(x) == "cdata" and tonumber(x) == 1 and ffi.typeof(x))
end
do
  local buffer = require("string.buffer")
  local mt = {}
  local dict = { "key", "hello", "key", false }
  local dict_mt = { mt, mt, false }
  local b = buffer.new({ dict = dict, metatable = dict_mt })
  b:encode(setmetatable({ key = "hello" }, mt))
  local out = b:decode()
  assert(out.key == "hello" and getmetatable(out) == mt)
end
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
  'lj_tab_storetvn' \
  'lj_tab_storenilraw' \
  'tab_storekeyrel' \
  'copyTVrel(L, dst, src)' \
  'copyTVrel(L, &dst[i], &src[i])' \
  'copyTVrel(L, dst, &k)' \
  'copyTVrel(L, slot, &val)' \
  'copyTVrel(L, tab_rehash_insert(L, newnode, newhmask, &newfreetop, &key),' \
  'lib_storetv_key(L, tab, L->top+1, L->top)' \
  'copyTVrel(L, o, f)' \
  'table_insert_shift_store(L, t, i)' \
  'table_insert_value_store(L, t, i, L->top-1)' \
  'table_pack_storeint_str(L, t, strV(lj_lib_upvalue(L, 1)), (int32_t)n)' \
  'lj_tab_trystoretv_cas(L, dst, &val) == LJ_TAB_STORE_CAS_OK' \
  'lj_tab_storetv(L, &array[i], &base[i])' \
  'base_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "kv"))' \
  'base_storetab_str(L, env, lj_str_newlit(L, "_G"), env)' \
  'string_storetab_str(L, mt, mmname_str(g, MM_index), strtab)' \
  'ctype_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "k"))' \
  'gc_stats_storetv_str(L, t, name, &tv)' \
  'gc_stats_storetv_int(L, bt, (int32_t)i + 1, &tv)' \
  'gc_stats_storetv_str(L, t, "poll_ack_latency_buckets", &tv)' \
  'jit_util_storetv_str(L, t, lj_str_newz(L, name), &tv)' \
  'jit_util_storetv_int(L, t, key, &tv)' \
  'setprotofield(L, t, lj_str_newlit(L, "proto"), pt)' \
  'setintptrfield(L, t, lj_str_newlit(L, "addr"), (intptr_t)(void *)fn->c.f)' \
  'setintindex(L, t, 0, (int32_t)snap->ref - REF_BIAS)' \
  'debug_activelines_storebool(L, t, line)' \
  'rec_rbchash_ref_acq(RBCHashEntry *rbc)' \
  'rec_rbchash_pc_acq(RBCHashEntry *rbc)' \
  'rec_rbchash_pt_acq(RBCHashEntry *rbc)' \
  'rec_rbchash_publish(jit_State *J, TRef tr, const BCIns *pc)' \
  'setmrefrel(rbc->pc, pc)' \
  'setgcrefrel(rbc->pt, obj2gco(J->pt))' \
  'la_store32_rel(&rbc->ref, tref_ref(tr))' \
  'rec_rbchash_ref_acq(rbc)' \
  'rec_rbchash_pc_acq(rbc)' \
  'rec_rbchash_pt_acq(rbc)' \
  'rec_rbchash_publish(J, tr, J->pc)' \
  'rec_rbchash_publish(J, rc, pc)' \
  'rec_template_mark_nil(J, tpl, &key)' \
  'rec_template_mark_nil(J, tpl, &ix->keyv)' \
  'lj_tab_storenil(J->L, &node[i].val)' \
  'lj_tab_storenil(J->L, &array[i])' \
  'copyTVrel(L, o, base+2)' \
  'slot = lib_storefunc_str(L, tab, name, fn)' \
  'jit_profile_registry_store(L, registry, &key, &tv)' \
  'jit_profile_registry_store(L, registry, &key, niltv(L))' \
  'jit_attach_event_store(L, tabV(L->top-2), L->top-1, niltv(L))' \
  'lj_tab_storenilraw(&array[i])' \
  'lj_tab_storenilraw(&n->val)' \
  'lj_cdata_fin_storenil(L, tv)' \
  'ffi_loaded_store(L, t, name, L->top-1)' \
  'ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1)' \
  'ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "info"), (int32_t)info)' \
  'ffi_typeinfo_storestr(L, t, lj_str_newlit(L, "name"), name)' \
  'lj_tab_storenilraw(&n->key)' \
  'const_slot_store(o, fs->nkn)' \
  'const_slot_store(o, fs->nkgc)' \
  'parse_keep_storebool(L, ls->fs->kt, &key)' \
  'parse_keep_storebool(L, ls->fs->kt, tv)' \
  'lj_tab_storetv(ls->L, o, &tv)' \
  'lj_tab_storetv(ls->L, lj_tab_set(ls->L, t, &key), &tv)' \
  'copyTVrel(sbufL(sbx), o, &tv)' \
  'settabV(sbufL(sbx), &tv, t)' \
  'copyTVrel(L, o, &tv)' \
  'lj_tab_storetv(L, val, &tmp)' \
  'serialize_dict_storeint(L, dict, &tv, (int32_t)(i-1))'
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

if rg -n 'copyTV\(L, (slot|tab_rehash_insert|&freenode->key|&n->key)|[fn][a-z]*node->key\.u64 = 0|n->key\.u64 = 0' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table rehash/new-key publication must use release key/value stores" >&2
  exit 1
fi

if rg -n 'copyTV\(L, o, L->top\+1\)|copyTV\(L, o, --L->top\)|copyTV\(L, dst, &val\)|setnilV\(dst\)|copyTV\(L, &array\[i\], &base\[i\]\)' \
    "$ROOT/src/lj_api.c" "$ROOT/src/lib_table.c"; then
  echo "guardrail: API/table library direct table slot stores must release-publish" >&2
  exit 1
fi

if rg -n 'lj_tab_storeint\(L, lj_tab_setstr\(L, t, strV\(lj_lib_upvalue\(L, 1\)\)\)' \
    "$ROOT/src/lib_table.c"; then
  echo "guardrail: table.pack n field must CAS-publish" >&2
  exit 1
fi

if rg -n 'lj_tab_store(int|intptr|proto)\(L, lj_tab_set(str|int)\(L, t' \
    "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.util result fields must CAS-publish" >&2
  exit 1
fi

if rg -n 'lj_tab_storebool\(L, lj_tab_setint\(L, t, line\), 1\)' \
    "$ROOT/src/lj_debug.c"; then
  echo "guardrail: debug activelines table must CAS-publish" >&2
  exit 1
fi

if rg -n 'settabV\(J->L, &(node\[i\]\.val|array\[i\]), tpl\)|settabV\(J->L, o, tpl\)|lj_tab_storetab\(J->L, (&node\[i\]\.val|o), tpl\)|setnilV\(&(node\[i\]\.val|array\[i\])\)' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder template table markers must release-publish" >&2
  exit 1
fi

if rg -n 'J->rbchash\[[^]]+\]\.ref = tref_ref|setmref\(J->rbchash|setgcref\(J->rbchash|mref\(rbc->pc|gcref\(rbc->pt' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder table-bump rollback cache must use acquire/release helpers" >&2
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

if rg -n 'o->u64 = fs->nk|lj_tab_storebool\(L,' "$ROOT/src/lj_parse.c"; then
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

if rg -n 'lj_tab_storeint\(L, lj_tab_newkey\(L, dict, &tv\)' \
    "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer dictionary indexes must use nil-only CAS helper" >&2
  exit 1
fi

if rg -n 'settabV\(J->L, o, t\)|snap_restoreval\(J, T, ex, snapno, rfilt, irs->op2, val\)|val->u32\.hi' \
    "$ROOT/src/lj_snap.c"; then
  echo "guardrail: snapshot table restore slots must release-publish" >&2
  exit 1
fi

echo "M5 table value publication guard passed"
