#!/bin/sh
# Guard the M6 helper-backed table store bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-jit-forward-store

make -C "$ROOT/src" >/dev/null

for needle in \
  'tab_ptr_index(uintptr_t base, uintptr_t elem,' \
  'tab_forwarded_jit_array_slot(lua_State *L, GCtab *parent' \
  'tab_forwarded_jit_hash_slot(GCtab *parent, TValue *dst,' \
  'dst = tab_forwarded_jit_array_slot(L, parent, orig);' \
  'dst = tab_forwarded_jit_hash_slot(parent, orig, &keycopy, &key);' \
  'lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK' \
  'JIT array store saw FORWARD after routing.' \
  'JIT hash store saw FORWARD after routing.' \
  'lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent' \
  'lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent' \
  'lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent' \
  'cTValue *key)' \
  'dst = lj_tab_set(L, parent, key);' \
  'JIT NEWREF store saw FORWARD after key resolve.' \
  'lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */' \
  'lj_gc2_barrier_weak_write(L, parent, NULL, dst);  /* M8: traced weak-value array write. */' \
  'lj_gc2_barrier_weak_write(L, parent, key, dst);  /* M8: traced weak hash write. */' \
  'lj_gc2_barrier_weak_write(L, parent, key, dst);  /* M8: traced NEWREF weak write. */' \
  'n = (Node *)orig;  /* Node.val is the first field. */' \
  'IRCALL_lj_tab_storetv_forjit_array' \
  'IRCALL_lj_tab_storetv_forjit_hash' \
  'IRCALL_lj_tab_storetv_forjit_newref' \
  'tabref = IR(xref->op1)->op1' \
  'xref->o == IR_NEWREF' \
  'id = IRCALL_lj_tab_storetv_forjit_newref' \
  'args[4] = ASMREF_TMP2;  /* cTValue *key */' \
  'IRTMPREF_IN2' \
  'emit_leatg(as, dest, tmptv2);' \
  'IRTMPREF_IN1|IRTMPREF_IN2' \
  'asm_ahstore_forjit(ASMState *as, IRIns *ir)' \
  '#if defined(__linux__) && LJ_TARGET_X64' \
  'IRRef lim = poll_alias_limit(J, xref);' \
  'M6: numeric NEWREF/HSTORE uses the generic returned-slot helper.' \
  'M6: previous-nil in-bounds ASTORE/HSTORE uses the helper bridge.' \
  'lj_tab_storetv_forjit_newref(L, t, &oldarray[key], &src, &keytv);' \
  'lj_tab_storetv_forjit_newref(L, t, &oldn->val, &src, &keytv);' \
  'exercise_newref_array_retiring_jit(L)' \
  'exercise_newref_hash_retiring_jit(L)' \
  't-jit-forward-store OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h" \
      "$ROOT/src/lj_ircall.h" "$ROOT/src/lj_asm_x86.h" \
      "$ROOT/src/lj_record.c" "$ROOT/src/lj_opt_mem.c" \
      "$ROOT/tests/t-jit-forward-store.c"; then
    echo "guardrail: missing table-store helper marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /lj_tab_storetv_forjit_array\(lua_State \*L, GCtab \*parent,/ {
    inarray = 1
  }
  inarray && /copyTVrel\(L, dst, src\)/ { raw = 1 }
  inarray && /tab_forwarded_jit_array_slot\(L, parent, orig\)/ { route = 1 }
  inarray && /lj_tab_trystoretv_cas\(L, dst, src\)/ { cas = 1 }
  inarray && /JIT array store saw FORWARD after routing/ { retry = 1 }
  inarray && /^}/ { inarray = 0 }
  END { exit raw || !route || !cas || !retry ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: JIT array table-store helper must CAS-retry late FORWARD" >&2
  exit 1
fi

if awk '
  /lj_tab_storetv_forjit_hash\(lua_State \*L, GCtab \*parent,/ {
    inhash = 1
  }
  inhash && /copyTVrel\(L, dst, src\)/ { raw = 1 }
  inhash && /tab_forwarded_jit_hash_slot\(parent, orig, &keycopy, &key\)/ { route = 1 }
  inhash && /lj_tab_trystoretv_cas\(L, dst, src\)/ { cas = 1 }
  inhash && /JIT hash store saw FORWARD after routing/ { retry = 1 }
  inhash && /^}/ { inhash = 0 }
  END { exit raw || !route || !cas || !retry ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: JIT hash table-store helper must CAS-retry late FORWARD" >&2
  exit 1
fi

if awk '
  /lj_tab_storetv_forjit_newref\(lua_State \*L, GCtab \*parent,/ {
    innewref = 1
  }
  innewref && /copyTVrel\(L, dst, src\)/ { raw = 1 }
  innewref && /dst = lj_tab_set\(L, parent, key\)/ { resolve = NR }
  innewref && /lj_tab_trystoretv_cas\(L, dst, src\)/ { cas = NR }
  innewref && /JIT NEWREF store saw FORWARD after key resolve/ { retry = 1 }
  innewref && /lj_gc2_barrier_weak_write\(L, parent, key, dst\)/ { weak = 1 }
  innewref && /^}/ { innewref = 0 }
  END { exit raw || !cas || !resolve || resolve > cas || !retry || !weak ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: JIT NEWREF table-store helper must resolve by key before CAS retry" >&2
  exit 1
fi

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-forward-store.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -e '
local util = require("jit.util")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(util.traceinfo(1), "shared existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hhole = { stable = 0 }
hhole.stable = nil
for i = 1, 200 do
  hhole.stable = i
  hhole.stable = nil
end
assert(hhole.stable == nil)
assert(util.traceinfo(1), "previous-nil hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(util.traceinfo(1), "shared existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local ahole = { 0, nil, 0 }
for i = 1, 200 do
  ahole[2] = i
  ahole[2] = nil
end
assert(ahole[2] == nil)
assert(util.traceinfo(1), "previous-nil array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wk = setmetatable({ stable = 0 }, { __mode = "k" })
for i = 1, 200 do
  wk.stable = i
end
assert(wk.stable == 200)
assert(util.traceinfo(1), "weak-key existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wv = setmetatable({ 0 }, { __mode = "v" })
for i = 1, 200 do
  wv[1] = i
end
assert(wv[1] == 200)
assert(util.traceinfo(1), "weak-value existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function hash_insert(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local hi = hash_insert(80)
assert(hi.stable == 80)
assert(util.traceinfo(1), "trace-local hash insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local ai = array_insert(80)
assert(ai[1] == 80)
assert(util.traceinfo(1), "trace-local array insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function phi_store(n)
  local a = { stable = 0 }
  local b = { stable = 0 }
  local t = a
  for i = 1, n do
    if i == 1 then t = a else t = b end
    t.stable = i
  end
  return a.stable + b.stable
end
assert(phi_store(80) == 81)
assert(util.traceinfo(1), "PHI-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local up = { stable = 0 }
local function upvalue_store(n)
  for i = 1, n do
    up.stable = i
  end
  return up.stable
end
assert(upvalue_store(80) == 80)
assert(util.traceinfo(1), "upvalue-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_escaped_store()
  local sink
  return function(n)
    for i = 1, n do
      local t = { stable = 0 }
      sink = t
      t.stable = i
    end
    return sink.stable
  end
end
local escaped_store = make_escaped_store()
assert(escaped_store(80) == 80)
assert(util.traceinfo(1), "closed-upvalue escaped existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_nested_escape()
  local sink
  return function(n)
    for i = 1, n do
      local outer = { inner = false }
      local t = { stable = 0 }
      outer.inner = t
      sink = outer
      t.stable = i
    end
    return sink.inner.stable
  end
end
local nested_escape = make_nested_escape()
assert(nested_escape(80) == 80)
assert(util.traceinfo(1), "nested escaped existing table store did not trace")
'

HASH_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-hstore-ir.XXXXXX")
NEW_HASH_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-new-hstore-ir.XXXXXX")
ARRAY_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-astore-ir.XXXXXX")
NEW_ARRAY_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-new-array-hstore-ir.XXXXXX")
OLD_NIL_HASH_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-oldnil-hstore-ir.XXXXXX")
OLD_NIL_ARRAY_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-oldnil-astore-ir.XXXXXX")
trap 'rm -f "$HASH_IR" "$NEW_HASH_IR" "$ARRAY_IR" "$NEW_ARRAY_IR" "$OLD_NIL_HASH_IR" "$OLD_NIL_ARRAY_IR"' EXIT HUP INT TERM

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = { stable = 0 }
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local hash store did not trace")
' > "$HASH_IR"

if ! grep -q 'TDUP' "$HASH_IR" || ! grep -q 'HSTORE' "$HASH_IR" ||
   ! grep -q 'XPOLL' "$HASH_IR"; then
  echo "guardrail: trace-local hash store must record TDUP/HSTORE with XPOLL" >&2
  cat "$HASH_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local new hash store did not trace")
' > "$NEW_HASH_IR"

if ! grep -q 'TNEW' "$NEW_HASH_IR" || ! grep -q 'NEWREF' "$NEW_HASH_IR" ||
   ! grep -q 'HSTORE' "$NEW_HASH_IR" || ! grep -q 'XPOLL' "$NEW_HASH_IR"; then
  echo "guardrail: trace-local new hash store must record TNEW/NEWREF/HSTORE with XPOLL" >&2
  cat "$NEW_HASH_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
h.stable = nil
for i = 1, 80 do
  h.stable = i
  h.stable = nil
end
assert(h.stable == nil)
assert(util.traceinfo(1), "previous-nil hash store did not trace")
' > "$OLD_NIL_HASH_IR"

if ! grep -q 'HSTORE' "$OLD_NIL_HASH_IR" ||
   ! grep -q 'TBAR' "$OLD_NIL_HASH_IR" ||
   ! grep -q 'XPOLL' "$OLD_NIL_HASH_IR"; then
  echo "guardrail: previous-nil hash store must record HSTORE/TBAR with XPOLL" >&2
  cat "$OLD_NIL_HASH_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = { 0 }
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local array store did not trace")
' > "$ARRAY_IR"

if ! grep -q 'TDUP' "$ARRAY_IR" || ! grep -q 'ASTORE' "$ARRAY_IR" ||
   ! grep -q 'XPOLL' "$ARRAY_IR"; then
  echo "guardrail: trace-local array store must record TDUP/ASTORE with XPOLL" >&2
  cat "$ARRAY_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local new numeric store did not trace")
' > "$NEW_ARRAY_IR"

if ! grep -q 'TNEW' "$NEW_ARRAY_IR" || ! grep -q 'NEWREF' "$NEW_ARRAY_IR" ||
   ! grep -q 'HSTORE' "$NEW_ARRAY_IR" || ! grep -q 'XPOLL' "$NEW_ARRAY_IR"; then
  echo "guardrail: trace-local new numeric store must record TNEW/NEWREF/HSTORE with XPOLL" >&2
  cat "$NEW_ARRAY_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local t = {}
for k in pairs(package) do
  local s = tostring(k)
  t[#t+1] = s
  assert(t[#t] == s and type(t[#t]) == "string",
	 "numeric NEWREF helper crossed src/key TValue temps")
end
assert(util.traceinfo(1), "numeric NEWREF append did not trace")
'

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0, nil, 0 }
for i = 1, 80 do
  a[2] = i
  a[2] = nil
end
assert(a[2] == nil)
assert(util.traceinfo(1), "previous-nil array store did not trace")
' > "$OLD_NIL_ARRAY_IR"

if ! grep -q 'ASTORE' "$OLD_NIL_ARRAY_IR" ||
   ! grep -q 'XPOLL' "$OLD_NIL_ARRAY_IR"; then
  echo "guardrail: previous-nil array store must record ASTORE with XPOLL" >&2
  cat "$OLD_NIL_ARRAY_IR" >&2
  exit 1
fi

SHARED_HASH_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-shared-hstore-ir.XXXXXX")
SHARED_ARRAY_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-shared-astore-ir.XXXXXX")
trap 'rm -f "$HASH_IR" "$NEW_HASH_IR" "$ARRAY_IR" "$NEW_ARRAY_IR" "$OLD_NIL_HASH_IR" "$OLD_NIL_ARRAY_IR" "$SHARED_HASH_IR" "$SHARED_ARRAY_IR"' EXIT HUP INT TERM

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i
end
assert(h.stable == 80)
assert(util.traceinfo(1), "shared existing hash store did not trace")
' > "$SHARED_HASH_IR"

if ! grep -q 'HSTORE' "$SHARED_HASH_IR" || ! grep -q 'XPOLL' "$SHARED_HASH_IR"; then
  echo "guardrail: shared existing hash store must record HSTORE with XPOLL" >&2
  cat "$SHARED_HASH_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0 }
for i = 1, 80 do
  a[1] = i
end
assert(a[1] == 80)
assert(util.traceinfo(1), "shared existing array store did not trace")
' > "$SHARED_ARRAY_IR"

if ! grep -q 'ASTORE' "$SHARED_ARRAY_IR" || ! grep -q 'XPOLL' "$SHARED_ARRAY_IR"; then
  echo "guardrail: shared existing array store must record ASTORE with XPOLL" >&2
  cat "$SHARED_ARRAY_IR" >&2
  exit 1
fi

echo "M6 JIT table-store helper guard passed"
