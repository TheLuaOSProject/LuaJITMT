/*
** Direct C contract for the strict ARM64 root-entry helper and VM replay.
** A real-prototype-backed synthetic trace reaches direct helper success, but
** its inert machine-code target is never executed here.
*/

#include <assert.h>
#include <stddef.h>
#include <pthread.h>
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
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-root-entry requires the granular integer-loop gate split"
#endif

typedef enum RootEntryRaceMode {
  ROOT_ENTRY_CLOSER_BEFORE_PUBLISH,
  ROOT_ENTRY_CLOSER_AFTER_PUBLISH,
  ROOT_ENTRY_REQUEST_AFTER_PUBLISH,
  ROOT_ENTRY_POLL_AFTER_METADATA,
  ROOT_ENTRY_REQMASK_AFTER_METADATA,
  ROOT_ENTRY_PROFILE_AFTER_METADATA
} RootEntryRaceMode;

typedef struct RootEntryRace {
  global_State *g;
  TGState *tg;
  RootEntryRaceMode mode;
  uint32_t entry_done;
  uint32_t worker_done;
  uint32_t saw_active;
  int entered;
} RootEntryRace;

typedef struct RootEntryPatch {
  BCIns *pc;
  BCIns original;
} RootEntryPatch;

typedef struct RootEntryNumericPatch {
  RootEntryPatch forl;
  BCIns *fori_pc;
  BCIns fori_original;
} RootEntryNumericPatch;

typedef struct RootEntryTraceVec2 {
  MSize sizetrace;
  uint64_t retire_epoch;
  TraceVec *retired_next;
  GCRef slot[2];
} RootEntryTraceVec2;

typedef struct RootEntryMetadataFixture {
  GCtrace saved_cur;
  TraceVec *saved_tracev;
  MSize saved_sizetrace;
  RootEntryTraceVec2 tracev;
  IRIns ir[2];
  SnapShot snap[1];
  SnapEntry snapmap[2];
  MCode mcode[4];
} RootEntryMetadataFixture;

typedef struct RootEntryFrameFixture {
  TValue saved_func;
} RootEntryFrameFixture;

static void run_lua(lua_State *L, const char *chunk)
{
  if (luaL_dostring(L, chunk) != 0) {
    fprintf(stderr, "root-entry Lua setup failed: %s\n",
	    lua_tostring(L, -1));
    assert(0);
  }
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  return funcproto(global_lfunc(L, name));
}

static RootEntryPatch patch_first_root(lua_State *L, const char *name,
				       BCOp originalop, BCOp jitop)
{
  GCproto *pt = global_proto(L, name);
  BCIns *bc = proto_bc(pt);
  BCPos i;
  RootEntryPatch patch = { NULL, 0 };
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == originalop) {
      patch.pc = &bc[i];
      patch.original = ins;
      proto_jit_startins_rel(pt, patch.pc, ins);
      bc_publish(patch.pc, BCINS_AD(jitop, bc_a(ins), 1));
      return patch;
    }
  }
  assert(!"root-entry fixture opcode not found");
  return patch;
}

static void restore_root_patch(RootEntryPatch *patch)
{
  assert(patch != NULL && patch->pc != NULL);
  bc_publish(patch->pc, patch->original);
}

static RootEntryNumericPatch patch_numeric_root(lua_State *L,
						 const char *name, int patch_fori)
{
  RootEntryNumericPatch patch;
  patch.forl = patch_first_root(L, name, BC_FORL, BC_JFORL);
  patch.fori_pc = patch.forl.pc + bc_j(patch.forl.original);
  patch.fori_original = (BCIns)la_load32_acq(
    (const uint32_t *)patch.fori_pc);
  assert(bc_op(patch.fori_original) == BC_FORI);
  if (patch_fori) {
    bc_publish(patch.fori_pc,
      BCINS_AD(BC_JFORI, bc_a(patch.fori_original),
	       bc_d(patch.fori_original)));
  } else {
    patch.fori_pc = NULL;
  }
  return patch;
}

static void restore_numeric_patch(RootEntryNumericPatch *patch)
{
  if (patch->fori_pc != NULL)
    bc_publish(patch->fori_pc, patch->fori_original);
  restore_root_patch(&patch->forl);
}

static void install_root_entry_frame(lua_State *L, GCfunc *fn,
				     RootEntryFrameFixture *fixture)
{
  assert(fn != NULL && isluafunc(fn));
  copyTV(L, &fixture->saved_func, L->base-2);
  setfuncV(L, L->base-2, fn);
  assert(curr_func(L) == fn);
}

static void remove_root_entry_frame(lua_State *L,
				    const RootEntryFrameFixture *fixture)
{
  copyTV(L, L->base-2, &fixture->saved_func);
}

static void install_root_entry_metadata(jit_State *J, GCproto *pt,
					const RootEntryPatch *loop,
					RootEntryMetadataFixture *fixture)
{
  memset(fixture, 0, sizeof(*fixture));
  fixture->saved_cur = J->cur;
  fixture->saved_tracev = tracevec_acq(J);
  fixture->saved_sizetrace = trace_sizetrace_acq(J);
  assert(fixture->saved_tracev == NULL);
  assert(offsetof(RootEntryTraceVec2, slot) == offsetof(TraceVec, slot));

  memset(&J->cur, 0, sizeof(J->cur));
  J->cur.gct = (uint32_t)~LJ_TTRACE;
  trace_traceno_rel(&J->cur, 1);
  J->cur.root = 0;
  trace_link_rel(&J->cur, 1);
  J->cur.linktype = LJ_TRLINK_LOOP;
  trace_nextside_rel(&J->cur, 0);
  J->cur.nchild = 0;
  J->cur.spadjust = 0;
  J->cur.topslot = pt->framesize;
  trace_startpt_rel(&J->cur, pt);
  setmref(J->cur.startpc, loop->pc);
  J->cur.startins = loop->original;
  J->cur.ir = fixture->ir;
  J->cur.nins = REF_FIRST+1;
  J->cur.nk = REF_TRUE;
  J->cur.snap = fixture->snap;
  J->cur.snapmap = fixture->snapmap;
  J->cur.nsnap = 1;
  J->cur.nsnapmap = 2;
  J->cur.unused1 = TRACE_ARM64_INT_LOOP_ADMITTED;
  fixture->mcode[0] = 0xd503201fu;  /* Unreachable AArch64 NOPs. */
  fixture->mcode[1] = 0xd503201fu;
  fixture->mcode[2] = 0xd503201fu;
  fixture->mcode[3] = 0xd503201fu;
  J->cur.szmcode = (MSize)sizeof(fixture->mcode);
  J->cur.mcode = fixture->mcode;
  J->cur.mcloop = (MSize)sizeof(MCode);
#if LJ_ABI_PAUTH
  J->cur.mcauth = lj_ptr_sign((ASMFunction)(void *)fixture->mcode, &J->cur);
#endif

  fixture->tracev.sizetrace = 2;
  fixture->tracev.retire_epoch = 0;
  fixture->tracev.retired_next = NULL;
  setgcrefrel(fixture->tracev.slot[0], NULL);
  setgcrefrel(fixture->tracev.slot[1], obj2gco(&J->cur));
  trace_sizetrace_rel(J, 2);
  tracevec_rel(J, (TraceVec *)&fixture->tracev);
}

static void remove_root_entry_metadata(jit_State *J,
				       RootEntryMetadataFixture *fixture)
{
  tracevec_rel(J, fixture->saved_tracev);
  trace_sizetrace_rel(J, fixture->saved_sizetrace);
  J->cur = fixture->saved_cur;
}

static void call_global(lua_State *L, const char *name, int nargs, int nresults)
{
  int base = lua_gettop(L) - nargs;
  lua_getglobal(L, name);
  lua_insert(L, base + 1);
  if (lua_pcall(L, nargs, nresults, 0) != 0) {
    fprintf(stderr, "root-entry call %s failed: %s\n", name,
	    lua_tostring(L, -1));
    assert(0);
  }
}

/* Kept out of line for the contract script: its call site proves that Clang's
** Darwin AAPCS64 lowering consumes LJTraceRootEntry.trace from x0 and target
** from x1, with no hidden result pointer. It is deliberately never executed. */
__attribute__((noinline, used))
GCtrace *lj_test_root_entry_abi_probe(jit_State *J, const BCIns *pc,
	TraceNo traceno, lua_State *L, TValue *base, uint32_t sourceop,
	ASMFunction *targetp)
{
  LJTraceRootEntry entry =
    lj_trace_enter_root(J, pc, traceno, L, base, sourceop);
  *targetp = entry.target;
  return entry.trace;
}

static void expect_reject(LJTraceRootEntry entry)
{
  assert(entry.trace == NULL);
  assert(entry.target == NULL);
}

static void expect_metadata_reject(lua_State *L, const BCIns *pc)
{
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  expect_reject(lj_trace_enter_root(L2J(L), pc, 1, L, L->base, BC_JLOOP));
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
}

static void expect_metadata_success(lua_State *L, const BCIns *pc,
				    RootEntryMetadataFixture *fixture)
{
  lua_State *saved_tmpbuf_L = sbufL(&L->tg_hint->tmpbuf);
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  LJTraceRootEntry entry = lj_trace_enter_root(
    L2J(L), pc, 1, L, L->base, BC_JLOOP);
  assert(entry.trace == &L2J(L)->cur);
  assert(entry.target != NULL);
  assert((uintptr_t)lj_ptr_strip(entry.target) ==
	 (uintptr_t)(void *)fixture->mcode);
  assert(lj_tg_load_jit_base(L->tg_hint) == L->base);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  /* Direct C validation must not execute the inert target. Release its exact
  ** synthetic entry intent just as a native exit would. */
  lj_tg_store_jit_base(L->tg_hint, NULL);
  setsbufL(&L->tg_hint->tmpbuf, saved_tmpbuf_L);
}

static void test_metadata_mutation_rejections(lua_State *L, GCproto *pt,
					      RootEntryPatch *loop)
{
  jit_State *J = L2J(L);
  GCtrace *T = &J->cur;
  IRIns *ir = T->ir;
  SnapShot *snap = T->snap;
  BCIns current = (BCIns)la_load32_acq((const uint32_t *)loop->pc);

  trace_link_rel(T, 2);
  expect_metadata_reject(L, loop->pc);
  trace_link_rel(T, 1);

  trace_traceno_rel(T, 2);
  expect_metadata_reject(L, loop->pc);
  trace_traceno_rel(T, 1);

  T->root = 1;
  expect_metadata_reject(L, loop->pc);
  T->root = 0;

  T->linktype = LJ_TRLINK_ROOT;
  expect_metadata_reject(L, loop->pc);
  T->linktype = LJ_TRLINK_LOOP;

  T->nchild = 1;
  expect_metadata_reject(L, loop->pc);
  T->nchild = 0;

  trace_nextside_rel(T, 1);
  expect_metadata_reject(L, loop->pc);
  trace_nextside_rel(T, 0);

  T->spadjust = 16;
  expect_metadata_reject(L, loop->pc);
  T->spadjust = 0;

  T->mcloop = 0;
  expect_metadata_reject(L, loop->pc);
  T->mcloop = (MSize)sizeof(MCode);

  T->startins = BCINS_AJ(BC_LOOP, bc_a(loop->original), 0);
  expect_metadata_reject(L, loop->pc);
  T->startins = loop->original;

  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_LOOP_ADMITTED;
  expect_metadata_reject(L, loop->pc);
  T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;

  T->retire_epoch = 1;
  expect_metadata_reject(L, loop->pc);
  T->retire_epoch = 0;

  T->unused1 |= TRACE_ENTRY_INVALIDATED;
  expect_metadata_reject(L, loop->pc);
  T->unused1 &= (uint8_t)~TRACE_ENTRY_INVALIDATED;

  T->ir = NULL;
  expect_metadata_reject(L, loop->pc);
  T->ir = ir;

  T->snap = NULL;
  expect_metadata_reject(L, loop->pc);
  T->snap = snap;

  T->topslot = (uint8_t)(pt->framesize-1u);
  expect_metadata_reject(L, loop->pc);
  T->topslot = pt->framesize;

  bc_publish(loop->pc, BCINS_AD(BC_JLOOP, bc_a(current), 2));
  expect_metadata_reject(L, loop->pc);
  bc_publish(loop->pc, current);
}

static void wait_for_pause(uint32_t stage, const RootEntryRace *race)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() == stage)
      return;
    assert(la_load32_acq(&race->worker_done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"root-entry helper did not reach requested pause");
}

static uint32_t root_entry_race_stage(RootEntryRaceMode mode)
{
  if (mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH)
    return LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH;
  if (mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH ||
      mode == ROOT_ENTRY_REQUEST_AFTER_PUBLISH)
    return LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH;
  return LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA;
}

static void root_entry_publish_request(RootEntryRace *race)
{
  switch (race->mode) {
  case ROOT_ENTRY_POLL_AFTER_METADATA:
    lj_tg_poll_rel(race->tg, 1);
    break;
  case ROOT_ENTRY_REQMASK_AFTER_METADATA:
    lj_tg_reqmask_rel(race->tg, LJ_GC2_HS_REDISPATCH);
    break;
  case ROOT_ENTRY_REQUEST_AFTER_PUBLISH:
  case ROOT_ENTRY_PROFILE_AFTER_METADATA:
    lj_tg_profile_request_rel(race->tg, 1);
    break;
  default:
    assert(!"root-entry race has no request publication");
  }
}

static void *root_entry_closer(void *arg)
{
  RootEntryRace *race = (RootEntryRace *)arg;
  uint32_t stage = root_entry_race_stage(race->mode);
  wait_for_pause(stage, race);
  if (race->mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    uint32_t i;
    for (i = 0; i < 10000000u; i++) {
      if (lj_gc2_test_idle_reclaim_enter(race->g)) {
        race->entered = 1;
        break;
      }
      (void)lj_thr_retry_yield(NULL);
    }
    assert(race->entered == 1);
    assert(gc2_jit_phase_gate_acq(race->g) == 0);
    lj_trace_test_root_entry_release();
    while (la_load32_acq(&race->entry_done) == 0)
      la_cpu_pause();
    lj_gc2_test_idle_reclaim_leave(race->g);
  } else if (race->mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH) {
    race->saw_active = (uint32_t)lj_tg_any_jit_active(race->g);
    race->entered = lj_gc2_test_idle_reclaim_enter(race->g);
    if (race->entered)
      lj_gc2_test_idle_reclaim_leave(race->g);
    lj_trace_test_root_entry_release();
  } else {
    root_entry_publish_request(race);
    lj_trace_test_root_entry_release();
  }
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void require_idle_reclaim_preflight(global_State *g)
{
  if (!lj_gc2_test_idle_reclaim_enter(g)) {
    fprintf(stderr, "root-entry idle preflight failed: phase=%u gate=%u "
      "active=%d smr=%u\n", gc2_phase_acq(g),
      gc2_jit_phase_gate_acq(g), lj_tg_any_jit_active(g),
      gc2_smr_reclaiming_acq(g));
    assert(0);
  }
  lj_gc2_test_idle_reclaim_leave(g);
}

static void run_pause_race(lua_State *L, BCIns *pc, RootEntryRaceMode mode)
{
  global_State *g = G(L);
  TGState *tg = L->tg_hint;
  RootEntryRace race = { g, tg, mode, 0, 0, 0, 0 };
  pthread_t closer;
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  uint32_t stage = root_entry_race_stage(mode);
  LJTraceRootEntry entry;

  gc2_jit_sweep_displaced_rel(g, 0);
  lj_trace_test_root_entry_pause(stage);
  assert(pthread_create(&closer, NULL, root_entry_closer, &race) == 0);
  entry = lj_trace_enter_root(L2J(L), pc, 1, L, L->base, BC_JLOOP);
  expect_reject(entry);
  assert(lj_tg_load_jit_base(tg) == NULL);
  la_store32_rel(&race.entry_done, 1);
  assert(pthread_join(closer, NULL) == 0);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
  if (mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    assert(race.entered == 1);
    assert(gc2_jit_sweep_displaced_acq(g) == 0); /* Leave consumed it. */
  } else if (mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH) {
    assert(race.saw_active == 1);
    assert(race.entered == 0);
  } else if (mode == ROOT_ENTRY_REQUEST_AFTER_PUBLISH ||
	     mode == ROOT_ENTRY_PROFILE_AFTER_METADATA) {
    assert(lj_tg_profile_request_acq(tg) == 1);
    lj_tg_profile_request_rel(tg, 0);
  } else if (mode == ROOT_ENTRY_POLL_AFTER_METADATA) {
    assert(lj_tg_poll_acq(tg) == 1);
    lj_tg_poll_rel(tg, 0);
  } else {
    assert(mode == ROOT_ENTRY_REQMASK_AFTER_METADATA);
    assert(lj_tg_reqmask_acq(tg) == LJ_GC2_HS_REDISPATCH);
    lj_tg_reqmask_rel(tg, 0);
  }
  assert(gc2_jit_phase_gate_acq(g) != 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  jit_State *J;
  GCfunc *metadata_fn;
  GCproto *metadata_pt;
  RootEntryPatch metadata_loop;
  RootEntryFrameFixture frame_fixture;
  uint32_t publishes, cleanups;
  int32_t saved_vmstate;
  lua_State *tmpbuf_L;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = L->tg_hint;
  J = L2J(L);
  assert(g != NULL && tg != NULL && J != NULL);
  assert(tg == lj_thr_get_tg() && tg->gl == g);
  assert(lj_tg_load_cur_L(tg) == L && lj_tg_owns_state_acq(tg, L));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(tracevec_acq(J) == NULL); /* No root has recorded yet. */
  run_lua(L,
    "jit.off()\n"
    "function __arm64_root_metadata(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i + 1 end\n"
    "  return x\n"
    "end\n");
  metadata_fn = global_lfunc(L, "__arm64_root_metadata");
  metadata_pt = funcproto(metadata_fn);
  metadata_loop = patch_first_root(L, "__arm64_root_metadata",
				   BC_LOOP, BC_JLOOP);
  assert(bc_j(metadata_loop.original) > 0);
  assert(bc_op(metadata_loop.pc[bc_j(metadata_loop.original)]) == BC_JMP);
  tmpbuf_L = sbufL(&tg->tmpbuf);
  lua_gc(L, LUA_GCSTOP, 0);
  require_idle_reclaim_preflight(g);
  lj_trace_test_root_entry_reset();
  install_root_entry_frame(L, metadata_fn, &frame_fixture);
  /* A direct C fixture starts in ~LJ_VMST_C. Emulate the exact VM state in
  ** which BC_JLOOP invokes this helper, then restore the harness state before
  ** exercising the real VM callers below. */
  saved_vmstate = lj_tg_vmstate_load_acq(tg);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);

  /* Invalid calls reject before publication and never clear a foreign lease. */
  expect_reject(lj_trace_enter_root(NULL, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(
    (jit_State *)((char *)J + sizeof(void *)), metadata_loop.pc, 1,
    L, L->base,
    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, NULL, 1, L, L->base, BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 0, L, L->base,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, NULL, L->base,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, NULL,
                                    BC_JLOOP));
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JFUNCV));
  {
    TValue *savedbase = L->base;
    TValue *stack = mref_acq(L->stack, TValue);
    assert(stack != NULL && L->top >= stack + LJ_FR2);
    L->base = stack + LJ_FR2;
    expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                      BC_JLOOP));
    L->base = savedbase;
  }
  {
    TValue *savedbase = L->base;
    TValue *savedtop = L->top;
    TValue *stack = mref_acq(L->stack, TValue);
    TValue *maxstack = mref_acq(L->maxstack, TValue);
    TValue saved_func;
    assert(stack != NULL && maxstack != NULL && maxstack-stack >= 3);
    assert(metadata_pt->framesize > 1);
    copyTV(L, &saved_func, maxstack-3);
    setfuncV(L, maxstack-3, metadata_fn);
    L->base = maxstack-1;
    L->top = maxstack;
    assert(metadata_pt->framesize > (MSize)(maxstack-L->base));
    expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                      BC_JLOOP));
    L->base = savedbase;
    L->top = savedtop;
    copyTV(L, maxstack-3, &saved_func);
  }
  lj_tg_store_jit_base(tg, L->base);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == L->base);
  lj_tg_store_jit_base(tg, NULL);
  assert(lj_tg_vmstate_load_acq(tg) == (int32_t)~LJ_VMST_INTERP);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_C);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
  lj_tg_vmstate_store_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
  lj_tg_in_native_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_in_native_rel(tg, 0);
  lj_tg_poll_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_poll_rel(tg, 0);
  lj_tg_reqmask_rel(tg, LJ_GC2_HS_REDISPATCH);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_reqmask_rel(tg, 0);
  lj_tg_profile_request_rel(tg, 1);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_profile_request_rel(tg, 0);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* The open loop gate publishes intent before absent metadata reaches the
  ** one cleanup path. JFUNCF remains closed at the source gate and publishes
  ** no TG lifetime intent at all. */
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  publishes = lj_trace_test_root_entry_publishes();
  cleanups = lj_trace_test_root_entry_cleanups();
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JFUNCF));
  assert(lj_trace_test_root_entry_publishes() == publishes);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* A gate owner wins before publication: entry records displacement and does
  ** not claim a TG lifetime lease. */
  require_idle_reclaim_preflight(g);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  publishes = lj_trace_test_root_entry_publishes();
  cleanups = lj_trace_test_root_entry_cleanups();
  gc2_jit_sweep_displaced_rel(g, 0);
  expect_reject(lj_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(gc2_jit_sweep_displaced_acq(g) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);
  lj_gc2_test_idle_reclaim_leave(g);

  require_idle_reclaim_preflight(g);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_CLOSER_BEFORE_PUBLISH);
  require_idle_reclaim_preflight(g);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_CLOSER_AFTER_PUBLISH);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_REQUEST_AFTER_PUBLISH);
  {
    RootEntryMetadataFixture metadata;
    install_root_entry_metadata(J, metadata_pt, &metadata_loop, &metadata);
    expect_metadata_success(L, metadata_loop.pc, &metadata);
    test_metadata_mutation_rejections(L, metadata_pt, &metadata_loop);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_POLL_AFTER_METADATA);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_REQMASK_AFTER_METADATA);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_PROFILE_AFTER_METADATA);
    remove_root_entry_metadata(J, &metadata);
  }
  assert(tracevec_acq(J) == NULL && J->cur.traceno == 0);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);
  remove_root_entry_frame(L, &frame_fixture);
  restore_root_patch(&metadata_loop);
  lj_tg_vmstate_store_rel(tg, saved_vmstate);

  /* Execute the checked-in BC_JLOOP and BC_JFUNCF VM callers themselves.
  ** The immutable startins sidecar supplies deterministic rejection recovery;
  ** no TraceVec slot or native target exists. */
  run_lua(L,
    "jit.off()\n"
    "function __arm64_root_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i + 1 end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_fixed(a, b, c)\n"
    "  return a * 3, b == nil, c == nil\n"
    "end\n"
    "local function __arm64_iter(t, k)\n"
    "  k = k + 1\n"
    "  local v = t[k]\n"
    "  if v ~= nil then return k, v end\n"
    "end\n"
    "function __arm64_root_iter(t)\n"
    "  local x = 0\n"
    "  for _, v in __arm64_iter, t, 0 do x = x + v end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_jforl(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x * 10 + i end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_jfori(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x * 10 + i end\n"
    "  return x\n"
    "end\n");
  {
    RootEntryPatch loop = patch_first_root(L, "__arm64_root_loop",
					  BC_LOOP, BC_JLOOP);
    RootEntryPatch fixed = patch_first_root(L, "__arm64_root_fixed",
					   BC_FUNCF, BC_JFUNCF);
    RootEntryPatch iter = patch_first_root(L, "__arm64_root_iter",
					  BC_ITERL, BC_JITERL);
    RootEntryNumericPatch jforl = patch_numeric_root(
      L, "__arm64_root_jforl", 0);
    RootEntryNumericPatch jfori = patch_numeric_root(
      L, "__arm64_root_jfori", 1);
    uint32_t loop_publishes, loop_cleanups;

    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lj_trace_test_root_entry_retry_restore(loop.pc, loop.original);
    lua_pushinteger(L, 100);
    call_global(L, "__arm64_root_loop", 1, 1);
    assert(lua_tointeger(L, -1) == 5050);
    lua_pop(L, 1);
    loop_publishes = lj_trace_test_root_entry_publishes();
    loop_cleanups = lj_trace_test_root_entry_cleanups();
    assert(loop_publishes != 0 && loop_publishes == loop_cleanups);
    assert(lj_trace_test_root_entry_startins_calls() == 1);
    assert((BCIns)la_load32_acq((const uint32_t *)loop.pc) == loop.original);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 14);
    call_global(L, "__arm64_root_fixed", 1, 3);
    assert(lua_tointeger(L, -3) == 42);
    assert(lua_toboolean(L, -2) != 0);
    assert(lua_toboolean(L, -1) != 0);
    lua_pop(L, 3);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    /* Preserve the pre-existing JITERL -> JLOOP tail path: it recovers its
    ** ITERL startins directly and never enters the strict-root helper. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_createtable(L, 4, 0);
    lua_pushinteger(L, 3); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 6); lua_rawseti(L, -2, 2);
    lua_pushinteger(L, 9); lua_rawseti(L, -2, 3);
    lua_pushinteger(L, 12); lua_rawseti(L, -2, 4);
    call_global(L, "__arm64_root_iter", 1, 1);
    assert(lua_tointeger(L, -1) == 30);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    /* JFORL has already incremented and tested the index before its JLOOP
    ** tail. Recovery must branch with FORL.D without executing FORL again. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 4);
    call_global(L, "__arm64_root_jforl", 1, 1);
    assert(lua_tointeger(L, -1) == 1234);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    lua_pushnumber(L, 4.5);
    call_global(L, "__arm64_root_jforl", 1, 1);
    assert(lua_tonumber(L, -1) == 1234.0);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);

    /* A synthetic paired JFORI reaches the same JFORL PC before executing the
    ** first body. Branch-only recovery must neither skip i=1 nor double-step
    ** later JFORL edges. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 4);
    call_global(L, "__arm64_root_jfori", 1, 1);
    assert(lua_tointeger(L, -1) == 1234);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    lua_pushnumber(L, 4.5);
    call_global(L, "__arm64_root_jfori", 1, 1);
    assert(lua_tonumber(L, -1) == 1234.0);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    restore_numeric_patch(&jfori);
    restore_numeric_patch(&jforl);
    restore_root_patch(&iter);
    restore_root_patch(&fixed);
    restore_root_patch(&loop);
  }

  lj_trace_test_root_entry_reset();
  lua_close(L);
  puts("arm64_jit_root_entry OK: strict loop entry, source gates, mutations and request races verified");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_root_entry SKIP: requires native experimental macOS ARM64");
  return 0;
}

#endif
