/* Admission tests for CALLXS results held across another CALLXS. */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "held CALLXS result admission requires ARM64 CALLXS admission"
#endif

#define HELD_MAX_CALLS 8u

typedef struct HeldFixture {
  lua_State *L;
  jit_State *J;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, semantic_nins, ktrace;
  MSize nsnap, nsnapmap;
  IRRef calls[HELD_MAX_CALLS];
  unsigned ncalls;
  IRRef producer_call, consumer_call, held_value;
  IRType expected_type;
  GCtrace view_owner, semantic;
} HeldFixture;

typedef struct JSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef1 ktrace;
} JSave;

static const char *const runners[] = {
  "__held_run_int", "__held_run_num", "__held_run_float", "__held_run_u32"
};
static const char *const producers[] = {
  "__held_i32_producer", "__held_num_producer",
  "__held_float_producer", "__held_u32_producer"
};
static const char *const consumers[] = {
  "__held_i32_consumer", "__held_num_consumer",
  "__held_float_consumer", "__held_u32_consumer"
};
static unsigned semantic_positives, postra_positives, postra_negatives;

static int set_errno_caller(lua_State *L)
{
  UNUSED(L);
  errno = E2BIG;
  return 0;
}

static BCIns loadbc(const BCIns *pc)
{ return (BCIns)la_load32_acq((const uint32_t *)pc); }

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
}

static unsigned op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r;
  unsigned n = 0;
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++)
    if (ir_load_acq(&ir[r]).o == op) n++;
  return n;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq((GCtrace *)T);
  IRRef n = trace_nins_acq((GCtrace *)T);
  while (n > REF_BASE) {
    IRIns tail = ir_load_acq(&ir[n-1u]);
    if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL &&
        tail.op1 >= REF_FIRST && tail.op1 < n-1u &&
        tail.op2 < trace_nsnap_acq((GCtrace *)T) && tail.s == SPS_NONE)
      n--;
    else if (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
             tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0)
      n--;
    else
      break;
  }
  return n;
}

static IRRef find_ktrace(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r, nk = trace_nk_acq((GCtrace *)T);
  for (r = nk; r < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[r]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[r]) == obj2gco(T)) return r;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      r += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      r++;
    else
      return 0;
  }
  return 0;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T != NULL && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) == 4u && op_count(T, IR_XSAVE) == 4u)
      return T;
  }
  return NULL;
}

static void locate_calls(HeldFixture *fx)
{
  IRRef r;
  fx->ncalls = 0;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
    if (ir_load_acq(&fx->ir[r]).o != IR_CALLXS) continue;
    assert(fx->ncalls < HELD_MAX_CALLS);
    fx->calls[fx->ncalls++] = r;
  }
  assert(fx->ncalls == 4u);
}

static void reset_fixture(HeldFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner), full = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)full * sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
         (size_t)(full-nk) * sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)fx->nsnap * sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)fx->nsnapmap * sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  fx->semantic_nins = semantic_nins(fx->owner);
  locate_calls(fx);
}

static int ir_uses_after(const HeldFixture *fx, IRRef value, IRRef after)
{
  IRRef r;
  for (r = after + 1u; r < fx->nins; r++) {
    IRIns use = ir_load_acq(&fx->ir[r]);
    uint8_t mode = lj_ir_mode[use.o];
    if ((irm_op1(mode) == IRMref && use.op1 == value) ||
        (irm_op2(mode) == IRMref && use.op2 == value)) return 1;
  }
  return 0;
}

/* Find the actual producer value, including the exact FLOAT/U32-to-NUM
** conversion when the native return is not already Lua's numeric type. */
static void locate_held_value(HeldFixture *fx)
{
  fx->producer_call = fx->consumer_call = fx->held_value = 0;
  {
    IRRef producer = fx->calls[0], consumer = fx->calls[1], r;
    for (r = producer; r < consumer; r++) {
      IRIns value = ir_load_acq(&fx->ir[r]);
      int from_producer = r == producer ||
        (value.o == IR_CONV && value.op1 == producer);
      if (!from_producer || irt_type(value.t) != fx->expected_type) continue;
      if (!ir_uses_after(fx, r, consumer)) continue;
      fx->producer_call = producer;
      fx->consumer_call = consumer;
      fx->held_value = r;
      return;
    }
  }
  assert(!"trace has no producer result live across a later CALLXS");
}

static void init_fixture(lua_State *L, jit_State *J, HeldFixture *fx,
                         const char *runner, unsigned result_kind)
{
  fx->L = L; fx->J = J; fx->pt = global_proto(L, runner);
  fx->expected_type = result_kind == 0u ? IRT_INT : IRT_NUM;
  fx->owner = find_trace(J, fx->pt);
  if (fx->owner == NULL)
    fprintf(stderr, "held CALLXS trace missing for %s\n", runner);
  assert(fx->owner != NULL);
  fx->nins = trace_nins_acq(fx->owner);
  fx->nsnap = trace_nsnap_acq(fx->owner);
  fx->nsnapmap = trace_nsnapmap_acq(fx->owner);
  fx->ktrace = find_ktrace(fx->owner);
  assert(fx->ktrace != 0 && fx->nsnap != 0 && fx->nsnapmap != 0);
  fx->ir = (IRIns *)calloc((size_t)fx->nins, sizeof(IRIns));
  fx->snap = (SnapShot *)calloc((size_t)fx->nsnap, sizeof(SnapShot));
  fx->snapmap = (SnapEntry *)calloc((size_t)fx->nsnapmap, sizeof(SnapEntry));
  assert(fx->ir != NULL && fx->snap != NULL && fx->snapmap != NULL);
  reset_fixture(fx);
  locate_held_value(fx);
}

static JSave enter_semantic(HeldFixture *fx)
{
  jit_State *J = fx->J;
  JSave save = { J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace };
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->semantic_nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace+1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = 0;
  for (IRRef r = REF_FIRST; r < fx->semantic_nins; r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) { J->loopref = r; break; }
  assert(J->loopref != 0);
  J->parent = 0; J->exitno = 0; J->baseslot = 1u + LJ_FR2;
  J->framedepth = 0; J->retdepth = 0; J->ktrace = fx->ktrace;
  return save;
}

static void leave_semantic(jit_State *J, const JSave *s)
{
  J->curfinal = s->curfinal; J->pt = s->pt; J->startpc = s->startpc;
  J->loopref = s->loopref; J->parent = s->parent; J->exitno = s->exitno;
  J->baseslot = s->baseslot; J->framedepth = s->framedepth;
  J->retdepth = s->retdepth; J->ktrace = s->ktrace;
}

static LJArm64PostRAView postra_view(HeldFixture *fx)
{
  LJArm64PostRAView v = {0};
  v.ir = fx->ir; v.owner = &fx->view_owner; v.cts = ctype_ctsG(G(fx->L));
  v.snap = fx->snap; v.snapmap = fx->snapmap;
  v.proto_bc = proto_bc(fx->pt); v.proto = fx->pt;
  v.startpc = trace_startpc_acq(fx->owner); v.nins = trace_nins_acq(&fx->view_owner);
  v.nk = trace_nk_acq(fx->owner); v.nsnap = fx->nsnap; v.nsnapmap = fx->nsnapmap;
  v.spadjust = trace_spadjust_acq(fx->owner);
  v.proto_sizebc = fx->pt->sizebc; v.proto_numparams = fx->pt->numparams;
  v.root_topslot = trace_topslot_acq(fx->owner);
  v.startins = trace_startins_acq(fx->owner);
  return v;
}

static void assert_original(HeldFixture *fx)
{
  const BCIns *bc = proto_bc(fx->pt);
  uintptr_t start = (uintptr_t)(const void *)trace_startpc_acq(fx->owner);
  uintptr_t base = (uintptr_t)(const void *)bc;
  MSize index = (MSize)((start-base)/sizeof(BCIns));
  BCIns saved;
  JSave js;
  LJArm64PostRAView v;
  assert(index < fx->pt->sizebc);
  saved = loadbc(&bc[index]);
  assert(bc_op(saved) == BC_JFORL);
  bc_publish((const uint32_t *)&bc[index], trace_startins_acq(fx->owner));
  reset_fixture(fx);
  js = enter_semantic(fx);
  assert(lj_asm_arm64_ir_admit(fx->J, &fx->semantic, NULL));
  semantic_positives++;
  leave_semantic(fx->J, &js);
  bc_publish((const uint32_t *)&bc[index], saved);
  reset_fixture(fx);
  v = postra_view(fx);
  assert(lj_asm_arm64_postra_admit(&v, NULL));
  postra_positives++;
  locate_held_value(fx);
}

static int expect_postra(HeldFixture *fx, int accepted)
{
  LJArm64PostRAView v = postra_view(fx);
  int got = lj_asm_arm64_postra_admit(&v, NULL);
  if (got != accepted)
    fprintf(stderr, "held CALLXS post-RA expected %d, got %d\n", accepted, got);
  assert(got == accepted);
  if (accepted) postra_positives++; else postra_negatives++;
  return got;
}

static void assert_preserved_location(const HeldFixture *fx)
{
  IRIns value = ir_load_acq(&fx->ir[fx->held_value]);
  IRType type = irt_type(value.t);
  if (value.s != SPS_NONE) {
    MSize width = irt_is64(value.t) ? 2u : 1u;
    MSize capacity = SPS_FIXED + fx->view_owner.spadjust/sizeof(int32_t);
    assert((MSize)value.s >= SPS_FIRST && (MSize)value.s + width <= capacity);
    if (width == 2u)
      assert((((MSize)value.s-SPS_FIRST)&1u) == 0);
  } else if (type == IRT_INT) {
    assert(value.r >= RID_X19 && value.r <= RID_X28);
  } else {
    assert(type == IRT_NUM && value.r >= RID_D8 && value.r <= RID_D15);
  }
}

enum HeldMutation { HELD_CLOBBER, HELD_WRONG_BANK, HELD_SPILL_OUTSIDE,
                    HELD_NUM_SPILL_HALF };

static void reject_location_mutation(HeldFixture *fx, enum HeldMutation mutation)
{
  IRIns *value;
  IRType type;
  reset_fixture(fx);
  locate_held_value(fx);
  assert(fx->producer_call == fx->calls[0] &&
         fx->consumer_call == fx->calls[1]);
  assert_preserved_location(fx);
  value = &fx->ir[fx->held_value];
  type = irt_type(value->t);
  assert(type == IRT_INT || type == IRT_NUM);
  value->s = SPS_NONE;
  switch (mutation) {
  case HELD_CLOBBER:
    value->r = type == IRT_INT ? RID_RET : RID_FPRET;
    break;
  case HELD_WRONG_BANK:
    value->r = type == IRT_INT ? RID_D8 : RID_X19;
    break;
  case HELD_SPILL_OUTSIDE: {
    MSize capacity = SPS_FIXED + fx->view_owner.spadjust/sizeof(int32_t);
    value->r = RID_NONE;
    assert(capacity + 1u < SPS_LIMIT);
    value->s = (uint8_t)(capacity + 1u);
    assert((MSize)value->s > capacity);
    break;
  }
  case HELD_NUM_SPILL_HALF:
    assert(type == IRT_NUM);
    value->r = RID_NONE;
    value->s = SPS_FIRST + 1u;
    break;
  }
  (void)expect_postra(fx, 0);
}

static void run_case(lua_State *L, const char *runner, const char *producer,
                     const char *consumer)
{
  assert(luaL_dostring(L, "jit.flush()") == LUA_OK);
  lua_getglobal(L, "__held_configure");
  lua_pushinteger(L, 0);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
  lua_getglobal(L, "__held_driver");
  lua_getglobal(L, runner);
  lua_getglobal(L, producer);
  lua_getglobal(L, consumer);
  lua_pushinteger(L, 64);
  if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
    fprintf(stderr, "held CALLXS runner %s failed: %s\n", runner,
            lua_tostring(L, -1));
    abort();
  }
  assert(lua_isnumber(L, -1));
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  HeldFixture fx[4] = {{0}};
  unsigned i;
  lua_pushcfunction(L, set_errno_caller);
  lua_setglobal(L, "__held_set_errno");
  ljt_lua_dostring(L,
    "local ffi=require('ffi')\n"
    "ffi.cdef[[\n"
    "void lj_callxs_held_configure(int32_t);\n"
    "int32_t lj_callxs_held_producer_count(void);\n"
    "int32_t lj_callxs_held_consumer_count(void);\n"
    "int32_t lj_callxs_held_total_count(void);\n"
    "int32_t lj_callxs_held_first_errno(void);\n"
    "int32_t lj_callxs_held_last_errno(void);\n"
    "int32_t lj_callxs_held_i32(int32_t);\n"
    "double lj_callxs_held_num(int32_t);\n"
    "float lj_callxs_held_float(int32_t);\n"
    "uint32_t lj_callxs_held_u32(int32_t);\n"
    "int32_t lj_callxs_held_i32_consume(int32_t);\n"
    "double lj_callxs_held_num_consume(double);\n"
    "float lj_callxs_held_float_consume(float);\n"
    "double lj_callxs_held_u32_consume(double);\n"
    "]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_HELD_RESULTS_SO')))\n"
    "_G.__held_i32_producer=lib.lj_callxs_held_i32\n"
    "_G.__held_i32_consumer=lib.lj_callxs_held_i32_consume\n"
    "_G.__held_num_producer=lib.lj_callxs_held_num\n"
    "_G.__held_num_consumer=lib.lj_callxs_held_num_consume\n"
    "_G.__held_float_producer=lib.lj_callxs_held_float\n"
    "_G.__held_float_consumer=lib.lj_callxs_held_float_consume\n"
    "_G.__held_u32_producer=lib.lj_callxs_held_u32\n"
    "_G.__held_u32_consumer=lib.lj_callxs_held_u32_consume\n"
    "function _G.__held_configure(mode) lib.lj_callxs_held_configure(mode) end\n"
    "function _G.__held_run_int(producer,consumer,n) local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5 end\n"
    "function _G.__held_run_num(producer,consumer,n) local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5 end\n"
    "function _G.__held_run_float(producer,consumer,n) local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5 end\n"
    "function _G.__held_run_u32(producer,consumer,n) local sum=0.5; for i=1,n do local value=producer(i); local result=consumer(value); sum=sum+result+value end; return sum-0.5 end\n"
    "function _G.__held_driver(run,producer,consumer,n) __held_set_errno(); local result=run(producer,consumer,n); return result end\n"
    "jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  for (i = 0; i < 4; i++) {
    run_case(L, runners[i], producers[i], consumers[i]);
    init_fixture(L, J, &fx[i], runners[i], i);
    assert_original(&fx[i]);
    reject_location_mutation(&fx[i], HELD_CLOBBER);
    reject_location_mutation(&fx[i], HELD_WRONG_BANK);
    reject_location_mutation(&fx[i], HELD_SPILL_OUTSIDE);
    if (i != 0)
      reject_location_mutation(&fx[i], HELD_NUM_SPILL_HALF);
  }
  for (i = 0; i < 4; i++) {
    free(fx[i].snapmap); free(fx[i].snap); free(fx[i].ir);
  }
  lua_close(L);
  puts("t-arm64-jit-callxs-held-results-admission OK");
  printf("semantic=%u positive post-RA=%u positive/%u negative\n",
         semantic_positives, postra_positives, postra_negatives);
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-callxs-held-results-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}

#endif
