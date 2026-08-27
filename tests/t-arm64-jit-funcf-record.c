/*
** macOS ARM64 publication-only contract for the first fixed FUNCF root.
** The exact function(a, b) return true end trace may be recorded and
** published as JFUNCF, but the independently gated native header entry must
** reject before publishing a TG jit_base lifetime lease.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-funcf-record requires publication-only ARM64 FUNCF gates"
#endif

enum {
  FUNCF_R_SEPARATOR = REF_BASE+1,
  FUNCF_R_XPOLL,
  FUNCF_R_SUFFIX,
  FUNCF_R_END
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF record chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void push_integer_args(lua_State *L, int nargs, lua_Integer value)
{
  int i;
  for (i = 0; i < nargs; i++)
    lua_pushinteger(L, value+i);
}

static void call_expect_true(lua_State *L, const char *name, int nargs)
{
  int status;
  int top = lua_gettop(L);
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  push_integer_args(L, nargs, 40);
  status = lua_pcall(L, nargs, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF true call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isboolean(L, -1));
  assert(lua_toboolean(L, -1) != 0);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  assert(L->cframe == cframe);
}

static void expect_exact_true_bytecode(const GCproto *pt, BCIns *startp)
{
  const BCIns *bc = proto_bc(pt);
  BCIns start = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  BCIns kpri = (BCIns)la_load32_acq((const uint32_t *)&bc[1]);
  BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[2]);
  BCReg result;

  assert(pt->sizebc == 3);
  assert(pt->numparams == 2);
  assert(pt->framesize == 3);
  assert((pt->flags & PROTO_VARARG) == 0);
  result = (BCReg)(pt->framesize-1u);
  assert(bc_op(start) == BC_FUNCF);
  assert(bc_a(start) == pt->framesize && bc_d(start) == 0);
  assert(bc_op(kpri) == BC_KPRI);
  assert(bc_a(kpri) == result && bc_d(kpri) == 2u);
  assert(bc_op(ret) == BC_RET1);
  assert(bc_a(ret) == result && bc_d(ret) == 2u);
  *startp = start;
}

static int is_exact_true_bytecode(const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  BCIns start, kpri, ret;
  BCReg result;
  if (pt->sizebc != 3 || pt->numparams != 2 || pt->framesize == 0 ||
      (pt->flags & PROTO_VARARG) != 0)
    return 0;
  start = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  kpri = (BCIns)la_load32_acq((const uint32_t *)&bc[1]);
  ret = (BCIns)la_load32_acq((const uint32_t *)&bc[2]);
  result = (BCReg)(pt->framesize-1u);
  return bc_op(start) == BC_FUNCF && bc_a(start) == pt->framesize &&
	 bc_d(start) == 0 && bc_op(kpri) == BC_KPRI &&
	 bc_a(kpri) == result && bc_d(kpri) == 2u &&
	 bc_op(ret) == BC_RET1 && bc_a(ret) == result && bc_d(ret) == 2u;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  assert(ins.o == op);
  assert(ins.t.irt == type);
  assert(ins.op1 == op1);
  assert(ins.op2 == op2);
  assert(ins.s == SPS_NONE);
}

static void expect_exact_snapshots(const GCtrace *T, const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  uint64_t pcbase;
  MSize result_slot = (MSize)pt->framesize+LJ_FR2;

  assert(trace_nsnap_acq(T) == 2);
  assert(trace_nsnapmap_acq(T) == 5);
  assert(snap_ref_acq(&snap[0]) == FUNCF_R_SEPARATOR);
  assert(snap_mapofs_acq(&snap[0]) == 0);
  assert(snap_nent_acq(&snap[0]) == 0);
  assert(snap_nslots_acq(&snap[0]) == result_slot);
  assert(snap_topslot_acq(&snap[0]) == (MSize)pt->framesize);
  memcpy(&pcbase, &map[0], sizeof(pcbase));
  assert((uint8_t)pcbase == 0);
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == &bc[1]);

  assert(snap_ref_acq(&snap[1]) == FUNCF_R_XPOLL);
  assert(snap_mapofs_acq(&snap[1]) == 2);
  assert(snap_nent_acq(&snap[1]) == 1);
  assert(snap_nslots_acq(&snap[1]) == result_slot+1u);
  assert(snap_topslot_acq(&snap[1]) == (MSize)pt->framesize);
  assert(snapentry_acq(&map[2]) == SNAP(result_slot, 0, REF_TRUE));
  memcpy(&pcbase, &map[3], sizeof(pcbase));
  assert((uint8_t)pcbase == 0);
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == &bc[2]);
}

static void expect_exact_ir(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  assert(trace_nk_acq(T) == REF_TRUE);
  assert(trace_nins_acq(T) == FUNCF_R_END);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&ir[ref]);
    assert(k.o == IR_KPRI);
    assert(k.t.irt == (uint8_t)(REF_NIL-ref));
    assert(k.op12 == 0);
  }
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, FUNCF_R_SEPARATOR, IR_NOP, IRT_NIL, 0, 0);
  expect_ir(ir, FUNCF_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, FUNCF_R_SUFFIX, IR_NOP, IRT_NIL, 0, 0);
  assert(ir_load_acq(&ir[FUNCF_R_SUFFIX]).prev == 0);
}

static GCtrace *expect_published_true(lua_State *L, GCproto *pt,
	BCIns startins)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *bc = proto_bc(pt);
  BCIns live;
  TraceNo traceno;
  uint8_t flags;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 0);
  assert(trace_linktype_acq(T) == LJ_TRLINK_RETURN);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_startpc_acq(T) == &bc[0]);
  assert(trace_startins_acq(T) == startins);
  live = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  assert(live == BCINS_AD(BC_JFUNCF, bc_a(startins), 1));
  assert(proto_jit_startins_acq(pt, &bc[0]) == startins);

  flags = la_load8_acq(&T->unused1);
  assert(flags == TRACE_ARM64_TRUE_FUNCF_ADMITTED);
  assert((flags & (TRACE_ARM64_INT_LOOP_ADMITTED |
	 TRACE_ARM64_INT_FORL_ADMITTED)) == 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) > 0);
  assert(trace_mcloop_acq(T) == 0);
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  expect_exact_ir(T);
  expect_exact_snapshots(T, pt);

  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
  return T;
}

static GCtrace *hotcall_until_published(lua_State *L, const char *name)
{
  jit_State *J = L2J(L);
  unsigned i;
  for (i = 0; i < 256; i++) {
    /* Zero supplied arguments makes both fixed parameters genuinely missing
    ** on the C-driven hotcall which eventually starts the recorder. */
    call_expect_true(L, name, 0);
    if (trace_runnable_acq(traceref_safe(J, 1), 1))
      return traceref_safe(J, 1);
  }
  assert(!"fixed true FUNCF root did not publish");
  return NULL;
}

static void expect_closed_jfuncf_boundary(lua_State *L,
	uint32_t startins_calls)
{
  TGState *tg = L->tg_hint;
  assert(tg != NULL);
  assert(lj_trace_test_root_entry_startins_calls() == startins_calls);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

static void expect_closed_jfuncf_calls(lua_State *L, const char *name)
{
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  call_expect_true(L, name, 0);  /* Both fixed parameters missing. */
  expect_closed_jfuncf_boundary(L, 1);
  call_expect_true(L, name, 1);  /* One fixed parameter missing. */
  expect_closed_jfuncf_boundary(L, 2);
  call_expect_true(L, name, 2);  /* No fixed parameter missing. */
  /* This final recovered FUNCF hotcount underflows and statically redispatches
  ** the still-live JFUNCF once, accounting for the exact final delta of two. */
  expect_closed_jfuncf_boundary(L, 4);
}

static void run_positive(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n"
    "function __arm64_funcf_true(a, b) return true end\n");
  J = L2J(L);
  pt = global_proto(L, "__arm64_funcf_true");
  expect_exact_true_bytecode(pt, &startins);
  startpc = proto_bc(pt);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));

  T = hotcall_until_published(L, "__arm64_funcf_true");
  assert(T == expect_published_true(L, pt, startins));
  expect_closed_jfuncf_calls(L, "__arm64_funcf_true");

  /* jit.flush restores bytecode/slots; resetting the public hotloop parameter
  ** publishes a fresh owner-TG hotcount generation for deterministic reuse. */
  run_lua(L, "jit.flush(); jit.opt.start('hotloop=1')\n");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(proto_jit_startins_acq(pt, startpc) == startins);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);

  /* Full flush returns trace number one to the allocator. Republish the same
  ** immutable generation and re-prove that closed JFUNCF still interprets. */
  T = hotcall_until_published(L, "__arm64_funcf_true");
  assert(trace_traceno_acq(T) == 1);
  assert(trace_startpc_acq(T) == startpc);
  assert(T == expect_published_true(L, pt, startins));
  expect_closed_jfuncf_calls(L, "__arm64_funcf_true");
  lua_close(L);
}

typedef enum NegativeKind {
  NEG_FALSE,
  NEG_NIL,
  NEG_NUMBER,
  NEG_NONTRIVIAL
} NegativeKind;

static void call_negative(lua_State *L, const char *name, NegativeKind kind)
{
  int status;
  int nargs = kind == NEG_NONTRIVIAL ? 2 : 0;
  int top = lua_gettop(L);
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  if (kind == NEG_NONTRIVIAL) {
    lua_pushinteger(L, 17);
    lua_pushinteger(L, 17);
  }
  status = lua_pcall(L, nargs, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF negative call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  switch (kind) {
  case NEG_FALSE:
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0);
    break;
  case NEG_NIL:
    assert(lua_isnil(L, -1));
    break;
  case NEG_NUMBER:
    assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 42);
    break;
  case NEG_NONTRIVIAL:
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1) != 0);
    break;
  }
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  assert(L->cframe == cframe);
}

static void run_rejection(const char *name, const char *definition,
	NegativeKind kind)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  unsigned i;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n");
  run_lua(L, definition);
  J = L2J(L);
  pt = global_proto(L, name);
  assert(!is_exact_true_bytecode(pt));
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 256; i++)
    call_negative(L, name, kind);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  assert(bc_op((BCIns)la_load32_acq(
	(const uint32_t *)proto_bc(pt))) != BC_JFUNCF);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);
  lua_close(L);
}

static void run_rejections(void)
{
  run_rejection("__arm64_funcf_false",
    "function __arm64_funcf_false(a, b) return false end\n", NEG_FALSE);
  run_rejection("__arm64_funcf_nil",
    "function __arm64_funcf_nil(a, b) return nil end\n", NEG_NIL);
  run_rejection("__arm64_funcf_number",
    "function __arm64_funcf_number(a, b) return 42 end\n", NEG_NUMBER);
  run_rejection("__arm64_funcf_nontrivial",
    "function __arm64_funcf_nontrivial(a, b) return a == b end\n",
    NEG_NONTRIVIAL);
}

int main(void)
{
  run_positive();
  run_rejections();
  puts("arm64_jit_funcf_record OK: exact true FUNCF published; JFUNCF entry stayed closed");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_funcf_record SKIP: requires experimental macOS ARM64 JIT");
  return 0;
}

#endif
