#!/bin/sh
# Guard M5 JIT trace-slot and trace-link publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT_TRACEVEC=${TMPDIR:-/tmp}/lj_t-jit-tracevec
OUT_MCODE=${TMPDIR:-/tmp}/lj_t-jit-mcode-retire
OUT_TRACERET=${TMPDIR:-/tmp}/lj_t-jit-trace-retire

for needle in \
  'LJ_TRACE_PENDING' \
  'traceref_fromgco(GCobj *o)' \
  'TraceVec *tracev' \
  'TraceVec *retiredtracev' \
  'TraceVec *tv = tracevec_acq(J)' \
  'gcref_acq(tv->slot[(n)])' \
  'traceslot_pending(J, n)' \
  'traceslot_publish(J, n, T)' \
  'traceslot_clear(J, n)' \
  'traceno16_acq(const uint16_t *p)' \
  'trace_link_acq(T)' \
  'trace_nextroot_acq(T)' \
  'trace_nextside_acq(T)' \
  'proto_trace_acq(pt)' \
  'MCode **exittab' \
  'trace_exittarget_acq(T, exitno)' \
  'trace_exittarget_rel(T, exitno, target)' \
  'trace_startptgco_acq(GCtrace *T)' \
  'trace_startpt_acq(GCtrace *T)' \
  'trace_startpt_rel(GCtrace *T, GCproto *pt)' \
  'trace_startpt_clear(GCtrace *T)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_jit.h"; then
    echo "guardrail: missing trace publication helper: $needle" >&2
    exit 1
  fi
done

for needle in \
  'tracevec_new(lua_State *L, MSize sizetrace)' \
  'tracevec_publish(J, newtv)' \
  'tracevec_retire(J, oldtv)' \
  'GCtrace *retiredtraces' \
  'uint64_t retire_epoch' \
  'struct GCtrace *retired_next' \
  'uint64_t jit_scoped_slots_retired' \
  'la_store64_rlx(&g->gc2.jit_scoped_slots_retired, 0)' \
  'trace_retire(global_State *g, GCtrace *T)' \
  'lj_gc_arena_markmem(g, T)' \
	  'trace_freebody(global_State *g, GCtrace *T)' \
	  'lj_trace_free_unpublished(global_State *g, GCtrace *T)' \
	  'lj_trace_free_unpublished(J2G(J), J->curfinal)' \
	  'trace_markbody(global_State *g, GCtrace *T, int gc2)' \
  'lj_trace_reclaim_retired(global_State *g, uint64_t completed_epoch)' \
  'lj_trace_reclaim_retired(g, epoch)' \
  'lj_gc2_reclaim_retired(g, epoch)' \
  'lj_trace_markvecs(g, 1)' \
  'lj_trace_markvecs(g, 0)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_jit.h" \
	    "$ROOT/src/lj_trace.c" "$ROOT/src/lj_asm.c" \
    "$ROOT/src/lj_safepoint.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
    echo "guardrail: missing trace-vector RCU/SMR bridge: $needle" >&2
    exit 1
  fi
done

for needle in \
  'typedef struct MCodeRetire' \
  'MCodeRetire *retiredmcode' \
  'MCodeRetire *ret = lj_mem_newt(J->L, sizeof(MCodeRetire), MCodeRetire)' \
  'mcode_retired_push(jit_State *J, MCodeRetire *ret)' \
  'lj_mcode_reclaim_retired(global_State *g, uint64_t completed_epoch)' \
  'lj_mcode_reclaim_retired(g, epoch)' \
  'lj_gc2_reclaim_retired(g, epoch)' \
  'lj_mcode_freeretired(g)' \
  'lj_mcode_markretired(g, 1)' \
  'lj_mcode_markretired(g, 0)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_jit.h" "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_safepoint.c" "$ROOT/src/lj_trace.c" \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
    echo "guardrail: missing mcode SMR bridge: $needle" >&2
    exit 1
  fi
done

for needle in \
  'bc_publish(const uint32_t *pc, uint32_t ins)' \
  'la_store32_rel((uint32_t *)pc, ins)' \
  'bc_publish_op(const uint32_t *pc, BCOp op)' \
  'bc_publish_d(const uint32_t *pc, uint32_t d)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_bc.h"; then
    echo "guardrail: missing bytecode publication helper: $needle" >&2
    exit 1
  fi
done

for needle in \
  'EXITSTUB_TRACE_SPACING' \
  'exitstub_trace_addr(T, exitno)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_target_x86.h"; then
    echo "guardrail: missing x64 trace exit-stub helper: $needle" >&2
    exit 1
  fi
done

for needle in \
  'asm_exitstub_trace_setup(ASMState *as, ExitNo nexits)' \
  'mov rax, moffs64' \
  'xchg [rsp], rax; ret' \
  'exitstub_trace_addr(as->T, as->snapno)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing x64 trace exit indirection: $needle" >&2
    exit 1
  fi
done

for needle in \
  'trace_exittab_reset(jit_State *J, GCtrace *T)' \
  'trace_exittab_resetroot(J, T->traceno)' \
  'trace_exittab_reset(J, T);' \
  'lj_trace_flushall_hs(lua_State *L)' \
  'lj_trace_flushscope_hs(global_State *g, uint32_t work)' \
  'if (work != 0)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)' \
  'lj_trace_flushall(mainthread(g));  /* 08 section 8.7 leader action. */'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_trace.c" "$ROOT/src/lj_safepoint.c"; then
    echo "guardrail: missing x64 exittab flush reset: $needle" >&2
    exit 1
  fi
done

for needle in \
  'LJ_TRACE_SCOPE_FLUSHING' \
  'static uint32_t trace_flushroot(jit_State *J, GCtrace *T, int scoped)' \
  'uint32_t lj_trace_flush(jit_State *J, TraceNo traceno)' \
  'uint32_t lj_trace_flushproto(global_State *g, GCproto *pt)' \
  'la_store64_rel(&T->retire_epoch, LJ_TRACE_SCOPE_FLUSHING)' \
  'trace_scope_clear_slot(J, i, T, epoch);' \
  'lj_trace_flushscope_retire(global_State *g, uint64_t epoch)' \
  'uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)' \
  'root && root->traceno == T->root' \
  'T->traceno = 0;  /* Scoped slot retired after HS_EXIT_TRACES grace. */' \
  'epoch = la_load64_acq(&T->retire_epoch)' \
  'epoch == 0 || epoch == LJ_TRACE_SCOPE_FLUSHING' \
  'la_store64_rel(&T->retire_epoch, epoch)' \
  'la_add64_rlx(&g->gc2.jit_scoped_slots_retired, retired)' \
  'return (mode & LUAJIT_MODE_FLUSH) ? flushed : flushed + 1u;' \
  'return trace_flushroot(J, T, 1);' \
  'flushed += setptmode(g, pt, mode);' \
  'lj_trace_flushscope_hs(g, flushed);' \
  '(void)lj_trace_flushscope(G2J(g), idx);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_trace.c" "$ROOT/src/lj_dispatch.c"; then
    echo "guardrail: missing scoped trace flush handshake precision: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'scoped_epoch = la_load64_acq(&g->gc2.hs_epoch) + 1u;' \
    "$ROOT/tests/t-jit-trace-retire.c"; then
  echo "guardrail: trace retirement test must cover preserved scoped epochs" >&2
  exit 1
fi

if rg -F -q 'Temporary single-mutator flush action' "$ROOT/src/lj_safepoint.c"; then
  echo "guardrail: HS_FLUSHJ must be a leader action after ack drain" >&2
  exit 1
fi

if rg -F -q 'lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES);' "$ROOT/src/lj_dispatch.c"; then
  echo "guardrail: public scoped trace flushes must use count-aware scope helper" >&2
  exit 1
fi

hits=$(rg -n -g '*.c' -g '*.h' -g '!**/host/*' -- \
  'gcref\([^)]*startpt|setgcref\([^)]*startpt|setgcrefnull\([^)]*startpt' \
  "$ROOT/src" || true)
if [ -n "$hits" ]; then
  echo "guardrail: GCtrace.startpt must use trace_startpt acquire/release helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- '\blj_trace_flushall\(L\)' \
  "$ROOT/src/lj_api.c" "$ROOT/src/lj_dispatch.c" "$ROOT/src/lj_profile.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: public full flush callers must route through HS_FLUSHJ:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'setgcrefp\(J->trace|setgcrefnull\(J->trace|gcref\(J->trace' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_jit.h" || true)
if [ -n "$hits" ]; then
  echo "guardrail: J->trace slots must use acquire/release trace helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'lj_mem_growvec\(J->L, J->trace|lj_mem_freevec\(g, J->trace|gcref\(J->trace' \
  "$ROOT/src" "$ROOT/tests" || true)
if [ -n "$hits" ]; then
  echo "guardrail: trace vectors must use TraceVec RCU helpers, not raw slot vectors:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'pt->trace\b|->link\b|->nextroot\b|->nextside\b' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" \
  "$ROOT/src/lib_jit.c" "$ROOT/src/lj_bcwrite.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: shared trace-number fields must use acquire/release helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'setbc_op\(|setbc_d\(|setbc_j\(|\*J->patchpc[[:space:]]*=|\*pc[[:space:]]*=[[:space:]]*T->startins' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_record.c" "$ROOT/src/lj_dispatch.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: live bytecode patches must use full-word release publication:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'lj_asm_patchexit\(J, parent' "$ROOT/src/lj_trace.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: side traces must publish through exittab, not parent mcode patching:" >&2
  echo "$hits" >&2
  exit 1
fi

if ! awk '
  /static void trace_stop\(jit_State \*J\)/ { infn = 1 }
  infn && /lj_mcode_commit\(J, J->cur.mcode\)/ { commit = NR }
  infn && /trace_save\(J, T\)/ { save = NR }
  infn && /bc_publish\(patchpc, patchins\)/ { bc = NR }
  infn && /trace_exittarget_rel\(parent, J->exitno, T->mcode\)/ { side = NR }
  infn && /trace_link_rel\(parent, traceno\)/ { stitch = NR }
  END { exit(commit && save && bc && side && stitch &&
	     commit < save && save < bc && save < side && save < stitch ? 0 : 1) }
' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: trace_stop must publish final trace before bytecode/exit/link go-signals" >&2
  exit 1
fi

if ! rg -F -q 'lnk == as->T->traceno ? as->T : traceref(as->J, lnk)' \
  "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x86 assembler self-links must not read the pending trace slot" >&2
  exit 1
fi

if ! rg -F -q 'm5_jit_trace_publish.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: m5_jit_trace_publish.sh is not wired into the M5 aggregate" >&2
  exit 1
fi

make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-tracevec.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT_TRACEVEC"
timeout 20s "$OUT_TRACEVEC"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-mcode-retire.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT_MCODE"
timeout 20s "$OUT_MCODE"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-trace-retire.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT_TRACERET"
timeout 20s "$OUT_TRACERET"

"$ROOT/src/luajit" -e '
local util = require"jit.util"
local function tracecount()
  local n = 0
  for i = 1, 200 do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end
jit.off(tracecount, true)

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function f(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 40 do
  assert(f(200) == 20100)
end
assert(tracecount() > 0, "no root trace was published")
jit.flush()
assert(tracecount() == 0, "trace slots were not cleared")

jit.flush()
jit.opt.start("hotloop=1")
local function f1(a)
  if a > 0 then
    local b = f1(a - 1)
    return function()
      if type(b) == "function" then return a + b() end
      return a + b
    end
  end
  return a
end
local function f2(a) return f1(a)() end
for _ = 1, 41 do
  assert(f2(4) + f2(4) == 20)
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function side(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
local function expect(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
for _ = 1, 60 do
  assert(side(90, false) == expect(90, false))
end
local before = tracecount()
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(tracecount() > before, "no side trace was published")
local after_side = tracecount()
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(util.traceinfo(1), "missing root trace 1")
jit.flush(1)
assert(not util.traceinfo(1), "scoped root flush did not clear root slot")
assert(tracecount() < after_side, "scoped root flush did not retire any slots")
jit.flush()
assert(tracecount() == 0, "full flush after scoped root flush left traces")
for _ = 1, 20 do
  assert(side(90, true) == expect(90, true))
end
print("jit-trace-publish-smoke OK")
'

echo "M5 JIT trace publication guard passed"
