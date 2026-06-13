#!/bin/sh
# Guard M6 JIT recorder token scaffold.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-jit-token
DUMP=${TMPDIR:-/tmp}/lj_t-jit-xpoll.dump
FUNCF_DUMP=${TMPDIR:-/tmp}/lj_t-jit-xpoll-funcf.dump

make -C "$ROOT/src" >/dev/null

for needle in \
  'uint32_t jit_token' \
  'lj_jit_token_try(jit_State *J)' \
  'emit_leatg(as, dest, tmptv)' \
  'DISPATCH_TG(jit_base)' \
  'emit_gettg(as, tmp, gl)' \
  'XPOLL' \
  'emitir_raw(IRTG(IR_XPOLL, IRT_NIL), 0, 0)' \
  'LJ_TRACE_FUNCF_XPOLL_DEPTH' \
  'static void rec_func_xpoll(jit_State *J)' \
  'rec_func_xpoll(J)' \
  'case IR_XPOLL: asm_xpoll(as); break;' \
  'static void asm_xpoll(ASMState *as)' \
  'emit_gmroi(as, XG_ARITHi(XOg_CMP), RID_DISPATCH, DISPATCH_TG(poll), 0)' \
  'static int trace_poll_pending(lua_State *L)' \
  '!trace_poll_pending(L)' \
  'static void emit_pushx' \
  'static void emit_popx' \
  'static int asm_fuseggfref' \
  'static int asm_x86_isvmstate' \
  'la_cas32(&g->jit_token, &expect, tg->tid, LA_ACQ_REL, LA_ACQ)' \
  'void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)' \
  'lj_snap_restore_exit(jit_State *J, void *exptr, lua_State *L,' \
  'int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,' \
  'int jit_exitcode' \
  'G2TG(g)->jit_exitcode' \
  'tg->jit_exitcode' \
  'J->L = L;' \
  'lj_jit_token_held(J)' \
  'lj_jit_token_release(J)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_trace.h" \
      "$ROOT/src/lj_trace.c" "$ROOT/src/lj_dispatch.c" \
      "$ROOT/src/lj_snap.h" "$ROOT/src/lj_snap.c" "$ROOT/src/lj_tg.h" \
      "$ROOT/src/lj_err.c" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/src/lj_ir.h" "$ROOT/src/lj_opt_loop.c" \
      "$ROOT/src/lj_asm.c" "$ROOT/src/lj_emit_x86.h" \
      "$ROOT/src/lj_asm_x86.h" "$ROOT/src/lj_record.c"; then
    echo "guardrail: missing recorder token marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'while .*jit_token|la_futex_wait\(&g->jit_token|la_futex_wait\([^)]*jit_token' \
    "$ROOT/src"; then
  echo "guardrail: recorder token must never block or spin-wait" >&2
  exit 1
fi

if rg -n 'tg != g->main_tg|Temporary until x64 RID_DISPATCH addressing is localized|Secondary TGs interpret until RID_DISPATCH is local' \
    "$ROOT/src/lj_trace.c" "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: secondary TGs must be allowed to record and enter x64 traces" >&2
  exit 1
fi

if rg -n 'dispofs\(as, &J2TG\(as->J\)->(jit_base|tmptv|cur_L|gl)' \
    "$ROOT/src/lj_asm_x86.h" "$ROOT/src/lj_emit_x86.h"; then
  echo "guardrail: fixed TG fields must use DISPATCH_TG symbolic offsets" >&2
  exit 1
fi

if rg -n 'dispofs\(|J2TG\(as->J\)->dispatch|uint64_t dispaddr|GG_OFS_TGDISP' \
    "$ROOT/src/lj_asm_x86.h" "$ROOT/src/lj_emit_x86.h"; then
  echo "guardrail: generic x64 emitter must not use recorder-TG dispatch offsets" >&2
  exit 1
fi

if rg -n '\+\+snap->count' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: side-exit counters must not advance before token acquisition" >&2
  exit 1
fi

if awk '
  /->vm_hotloop:/ { hotloop = 1 }
  /->vm_callhook:/ { hotloop = 0 }
  /Stitch a new trace to the previous trace/ { stitch = 1 }
  /call extern lj_dispatch_stitch/ { stitch = 0 }
  (hotloop || stitch) && /DISPATCH_J\(L\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 hotloop/stitch must not write J->L before token acquisition" >&2
  exit 1
fi

if awk '
  /Stitch a new trace to the previous trace/ { stitch = 1 }
  /call extern lj_dispatch_stitch/ { stitch = 0 }
  stitch && /DISPATCH_J\(exitno\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 stitch must not write J->exitno before token acquisition" >&2
  exit 1
fi

if awk '
  /->vm_exit_handler:/ { exitpath = 1 }
  /->vm_exit_interp:/ { exitpath = 0 }
  exitpath && /\|\.if X64WIN/ { winonly = 1 }
  winonly && /\|\.else/ { winonly = 0 }
  winonly && /\|\.endif/ { winonly = 0 }
  exitpath && !winonly && /DISPATCH_J\((L|parent|exitno)\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64/POSIX trace exit restore state must stay call-local before side-token acquisition" >&2
  exit 1
fi

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-token.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
timeout 20s "$ROOT/src/luajit" "$ROOT/tests/t-jit-secondary.lua"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=im -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local s=0.0; for i=1,64 do s=s+i end; assert(s==2080.0)' \
  >"$DUMP"
if ! rg -q 'XPOLL' "$DUMP"; then
  echo "guardrail: x64 loop traces must materialize IR_XPOLL" >&2
  exit 1
fi
if ! rg -q -- '->LOOP:' "$DUMP" ||
   ! rg -q 'cmp dword \[r14\+0x[0-9a-f]+\], \+0x00' "$DUMP"; then
  echo "guardrail: x64 IR_XPOLL must lower to a TG poll at the loop label" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=im -e \
  'jit.opt.start("hotloop=1","hotexit=1","callunroll=32","recunroll=32"); local function f10(x) return x+1 end; local function f9(x) return f10(x)+1 end; local function f8(x) return f9(x)+1 end; local function f7(x) return f8(x)+1 end; local function f6(x) return f7(x)+1 end; local function f5(x) return f6(x)+1 end; local function f4(x) return f5(x)+1 end; local function f3(x) return f4(x)+1 end; local function f2(x) return f3(x)+1 end; local function f1(x) return f2(x)+1 end; local s=0; for i=1,64 do s=s+f1(i) end; assert(s==2720)' \
  >"$FUNCF_DUMP"
funcf_xpolls=$(rg -c 'XPOLL' "$FUNCF_DUMP" || true)
funcf_xpolls=${funcf_xpolls:-0}
if [ "$funcf_xpolls" -lt 4 ]; then
  echo "guardrail: deep inlined FUNCF traces must materialize depth XPOLL" >&2
  exit 1
fi
funcf_polls=$(rg -c 'cmp dword \[r14\+0x[0-9a-f]+\], \+0x00' "$FUNCF_DUMP" || true)
funcf_polls=${funcf_polls:-0}
if [ "$funcf_polls" -lt 4 ]; then
  echo "guardrail: FUNCF-depth IR_XPOLL must lower to TG poll checks" >&2
  exit 1
fi

echo "M6 JIT recorder token guard passed"
