#!/bin/sh
# Guard x64 closed-upvalue store publication routing.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for file in \
  "$ROOT/src/vm_x64.dasc" \
  "$ROOT/src/lj_asm_x86.h" \
  "$ROOT/src/lj_ircall.h"
do
  hits=$(rg -n 'lj_gc_barrieruv|IRCALL_lj_gc_barrieruv' "$file" || true)
  if [ -n "$hits" ]; then
    echo "guardrail: x64/JIT upvalue stores must use lj_gc_pubuv:" >&2
    echo "$hits" >&2
    exit 1
  fi
done

if ! awk '
  /void LJ_FASTCALL lj_gc_pubuv/ { infn = 1; seen = 1 }
  infn && /lj_tv_load_acq\(&snap, tv\)/ { snap = 1 }
  infn && /tvisgcv\(&snap\)/ { gcv = 1 }
  infn && /lj_gc2_barrier_tv_pair_g\(g, obj2gco\(uv\), &snap\)/ { gc2 = 1 }
  infn && /gc_mark\(g, gcV\(&snap\)\)/ { legacy = 1 }
  infn && /TV2MARKED\(tv\).*curwhite\(g\)/ { white = 1 }
  infn && /^}/ { exit(seen && snap && gcv && gc2 && legacy && white ? 0 : 1) }
  END { if (!seen || !snap || !gcv || !gc2 || !legacy || !white) exit 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: lj_gc_pubuv must preserve GC2 and legacy upvalue publication behavior" >&2
  exit 1
fi

for needle in \
  'call extern lj_func_storeuv_pub' \
  'call extern lj_func_storeuvstr_pub' \
  'lj_func_storeuv_pub(lua_State *L, TValue *tv, const TValue *src)' \
  'lj_func_storeuvstr_pub(lua_State *L, TValue *tv, GCstr *str)' \
  'IRCALL_lj_gc_pubuv' \
  'lj_gc_pubuv,' \
  'lj_func_storeuv_forjit,' \
  'IRCALL_lj_func_storeuv_forjit' \
  'asm_ustore_forjit' \
  'copyTVrel(L, tv, src);'
do
  if ! rg -F -q -- "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/src/lj_asm_x86.h" "$ROOT/src/lj_ircall.h" \
      "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"; then
    echo "guardrail: missing x64/JIT upvalue publication marker: $needle" >&2
    exit 1
  fi
done

if rg -F -n -- 'call extern lj_gc_pubuv' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 VM upvalue stores must publish through store helpers" >&2
  exit 1
fi

if ! rg -F -q -- 'lj_func_storeuv_forjit(lua_State *L, TValue *tv, const TValue *src)' \
    "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"; then
  echo "guardrail: JIT upvalue store helper must release-copy whole TValues" >&2
  exit 1
fi

storeuv_vm_count=$(awk '
  /call extern lj_func_storeuv_pub[[:space:]]*\/\/ \(lua_State \*L, TValue \*tv, const TValue \*src\)/ { n++ }
  END { print n + 0 }
' "$ROOT/src/vm_x64.dasc")
if [ "$storeuv_vm_count" -ne 2 ]; then
  echo "guardrail: x64 CSET/USETV closed-cell stores must use lj_func_storeuv_pub" >&2
  exit 1
fi

storeuvstr_vm_count=$(awk '
  /call extern lj_func_storeuvstr_pub[[:space:]]*\/\/ \(lua_State \*L, TValue \*tv, GCstr \*str\)/ { n++ }
  END { print n + 0 }
' "$ROOT/src/vm_x64.dasc")
if [ "$storeuvstr_vm_count" -ne 1 ]; then
  echo "guardrail: x64 USETS closed-cell stores must use lj_func_storeuvstr_pub" >&2
  exit 1
fi

if ! awk '
  /static void asm_ahustore\(ASMState \*as, IRIns \*ir\)/ {
    infn = 1
    next
  }
  infn && /ir->o == IR_USTORE && irt_isgcv\(ir->t\) && IR\(ir->op1\)->o == IR_UREFC/ {
    cond = NR
  }
  infn && /asm_ustore_forjit\(as, ir\)/ { helper = NR }
  infn && /if \(irt_isnum\(ir->t\)\)/ {
    raw = NR
    ok = cond && helper && cond < helper && helper < raw
    exit ok ? 0 : 1
  }
  END { if (!ok) exit 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 GC-valued UREFC stores must use the release-copy helper before raw stores" >&2
  exit 1
fi

echo "M5 x64 upvalue publication guard passed"
