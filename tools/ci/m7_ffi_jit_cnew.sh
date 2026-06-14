#!/bin/sh
# Guard M7 x64 JIT CNEW/CNEWI cdata publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
DUMP=${TMPDIR:-/tmp}/lj_t-ffi-jit-cnew.dump
DUMPI=${TMPDIR:-/tmp}/lj_t-ffi-jit-cnewi.dump

for needle in \
  'GCcdata *lj_cdata_new_forjit(lua_State *L, CTypeID id, CTSize sz)' \
  'lj_cdata_new_forjit,' \
  'IRCALL_lj_cdata_new_forjit' \
  'args[1] = ir->op1;      /* CTypeID id   */' \
  'emit_loadi(as, ra_releasetmp(as, ASMREF_TMP1), (int32_t)sz);' \
  'lj_cdata_new_forjit needs 3 args'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing JIT CNEW cdata allocation marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /static void asm_cnew\(ASMState \*as, IRIns \*ir\)/ { incnew = 1 }
  incnew && /^}/ { incnew = 0 }
  incnew && /IRCALL_lj_mem_newgco|offsetof\(GCcdata, marked\)|~LJ_TCDATA<<8/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 asm_cnew must not publish via generic GC alloc or write cdata header after publication" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" "$ROOT/tests/t-ffi-jit-cnew-alloc.lua"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    local ffi = require"ffi"
    ffi.cdef"typedef struct { int x; double y; } lj_m7_jit_dump_t;"
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1", "-sink")
    local struct_t = ffi.typeof("lj_m7_jit_dump_t")
    local int64_t = ffi.typeof("int64_t")
    local function make(n)
      local sum = 0
      for i = 1, n do
        local obj = struct_t(i, i + 0.25)
        local i64 = int64_t(i)
        sum = sum + obj.x + tonumber(i64)
      end
      return sum
    end
    for _ = 1, 30 do assert(make(80) == 6480) end
    print("dump cnew ok")
  ' \
  >"$DUMP"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    local ffi = require"ffi"
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1", "-sink")
    local int64_t = ffi.typeof("int64_t")
    local function make(n)
      local v = int64_t(0)
      for i = 1, n do v = int64_t(i) end
      return v
    end
    for _ = 1, 30 do assert(tonumber(make(80)) == 80) end
    print("dump cnewi ok")
  ' \
  >"$DUMPI"

if ! rg -q 'CNEW' "$DUMP"; then
  echo "guardrail: FFI JIT cdata allocation dump must materialize CNEW" >&2
  exit 1
fi

if ! rg -q 'CNEWI' "$DUMPI"; then
  echo "guardrail: FFI JIT cdata allocation dump must materialize CNEWI" >&2
  exit 1
fi

if ! rg -q 'dump cnew ok' "$DUMP" || ! rg -q 'dump cnewi ok' "$DUMPI"; then
  echo "guardrail: FFI JIT CNEW dump probes did not finish" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" \
  -j"$JOBS" >/dev/null

(
  cd "$ROOT/tests/stock/test"
  LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
    timeout 20s "$ROOT/src/luajit" test.lua --quiet 340 341 358
)

echo "M7 FFI JIT CNEW allocation guard passed"
