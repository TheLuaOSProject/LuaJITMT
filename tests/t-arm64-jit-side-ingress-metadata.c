/*
** Synthetic, read-only contract for the first ARM64 side-recording ingress
** certificate. The production side gate remains closed and this fixture never
** updates a snapshot count or starts the recorder.
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
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
#error "side ingress metadata checkpoint must land with recording closed"
#endif

enum {
  SIDE_META_PARENT = 1,
  SIDE_META_EXIT = 2,
  SIDE_META_PC_POS = 13,
  SIDE_META_NSNAP = 4,
  SIDE_META_NENT = 1,
  SIDE_META_FOOTER = 1 + LJ_FR2,
  SIDE_META_MAP_STRIDE = SIDE_META_NENT + SIDE_META_FOOTER,
  SIDE_META_NSNAPMAP = SIDE_META_NSNAP * SIDE_META_MAP_STRIDE,
  SIDE_META_BODY_WORDS = 4,
  SIDE_META_EXIT_WORDS = ARM64_EXIT_FALLBACK_WORDS +
    ARM64_EXIT_GATE_WORDS * SIDE_META_NSNAP,
  SIDE_META_MCODE_WORDS = SIDE_META_BODY_WORDS + SIDE_META_EXIT_WORDS,
  SIDE_META_R_VALUE = REF_FIRST,
  SIDE_META_R_END
};

typedef struct SideMetaTraceVec2 {
  MSize sizetrace;
  uint64_t retire_epoch;
  TraceVec *retired_next;
  GCRef slot[2];
} SideMetaTraceVec2;

typedef struct SideMetaFixture {
  GCtrace saved_cur;
  TraceNo saved_parent;
  ExitNo saved_exitno;
  TraceVec *saved_tracev;
  MSize saved_sizetrace;
  const BCIns *saved_pc;
  GCfunc *saved_fn;
  GCproto *saved_pt;
  int32_t saved_framedepth;
  int32_t saved_retdepth;
  BCReg saved_baseslot;
  TValue saved_frame_function;
  GCtrace parent_trace;
  SideMetaTraceVec2 tracev;
  IRIns ir[SIDE_META_R_END];
  SnapShot snap[SIDE_META_NSNAP];
  SnapEntry snapmap[SIDE_META_NSNAPMAP];
  _Alignas(8) MCode mcode[SIDE_META_MCODE_WORDS];
  _Alignas(8) MCode *exittab[SIDE_META_NSNAP];
  GCfunc *fn;
  GCproto *pt;
  BCIns *looppc;
  BCIns *backpc;
  BCIns loopins;
  BCIns backins;
} SideMetaFixture;

static void run_lua(lua_State *L, const char *chunk)
{
  if (luaL_dostring(L, chunk) != 0) {
    fprintf(stderr, "side metadata Lua setup failed: %s\n",
            lua_tostring(L, -1));
    assert(0);
  }
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static void side_meta_pack_pc(SnapEntry *dst, const BCIns *pc,
                              uint8_t base_delta)
{
#if LJ_FR2
  uint64_t pcbase = ((uint64_t)(uintptr_t)pc << 8) | base_delta;
  memcpy(dst, &pcbase, sizeof(pcbase));
#else
  assert(base_delta == 0);
  *dst = (SnapEntry)(uintptr_t)pc;
#endif
}

static void side_meta_setir(IRIns *ir, IRRef ref, IROp op, IRType type,
                            IRRef op1, IRRef op2)
{
  memset(&ir[ref], 0, sizeof(ir[ref]));
  ir[ref].op1 = (IRRef1)op1;
  ir[ref].op2 = (IRRef1)op2;
  ir[ref].t.irt = (uint8_t)type;
  ir[ref].o = (IROp1)op;
  ir[ref].r = RID_X0;
  ir[ref].s = SPS_NONE;
}

static void side_meta_find_loop(SideMetaFixture *f)
{
  BCIns *bc = proto_bc(f->pt);
  BCPos pos;
  for (pos = 0; pos < f->pt->sizebc; pos++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pos]);
    if (bc_op(ins) == BC_LOOP) {
      int64_t endpos = (int64_t)pos + (int64_t)bc_j(ins);
      assert(f->looppc == NULL && bc_j(ins) > 0);
      assert(endpos >= 0 && endpos < (int64_t)f->pt->sizebc);
      f->looppc = &bc[pos];
      f->loopins = ins;
      f->backpc = &bc[(BCPos)endpos];
      f->backins = (BCIns)la_load32_acq((const uint32_t *)f->backpc);
    }
  }
  assert(f->looppc != NULL && bc_op(f->backins) == BC_JMP &&
         bc_j(f->backins) < 0);
  assert(f->pt->sizebc > SIDE_META_PC_POS);
  assert(&bc[SIDE_META_PC_POS] >=
         f->backpc+1+bc_j(f->backins));
  assert(&bc[SIDE_META_PC_POS] < f->backpc+1);
}

static void side_meta_install(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  BCIns *bc;
  MCode *fallback, *gates;
  const BCPos footer_pc[SIDE_META_NSNAP] = { 3, 7, 13, 17 };
  MSize i;

  memset(f, 0, sizeof(*f));
  f->fn = global_lfunc(L, "__arm64_side_meta_f");
  f->pt = funcproto(f->fn);
  bc = proto_bc(f->pt);
  side_meta_find_loop(f);
  assert(proto_bcpos(f->pt, &bc[SIDE_META_PC_POS]) == SIDE_META_PC_POS);

  f->saved_cur = J->cur;
  f->saved_parent = J->parent;
  f->saved_exitno = J->exitno;
  f->saved_tracev = tracevec_acq(J);
  f->saved_sizetrace = trace_sizetrace_acq(J);
  f->saved_pc = J->pc;
  f->saved_fn = J->fn;
  f->saved_pt = J->pt;
  f->saved_framedepth = J->framedepth;
  f->saved_retdepth = J->retdepth;
  f->saved_baseslot = J->baseslot;
  copyTV(L, &f->saved_frame_function, L->base-2);
  setfuncV(L, L->base-2, f->fn);
  assert(curr_func(L) == f->fn);

  T->gct = (uint32_t)~LJ_TTRACE;
  trace_traceno_rel(T, SIDE_META_PARENT);
  trace_link_rel(T, SIDE_META_PARENT);
  T->root = 0;
  T->linktype = LJ_TRLINK_LOOP;
  trace_nextside_rel(T, 0);
  trace_nchild_rel(T, 0);
  trace_startpt_rel(T, f->pt);
  setmref(T->startpc, f->looppc);
  T->startins = f->loopins;
  T->topslot = f->pt->framesize;
  T->unused1 = TRACE_ARM64_INT_LOOP_ADMITTED;
  T->ir = f->ir;
  T->nk = REF_TRUE;
  T->nins = SIDE_META_R_END;
  T->snap = f->snap;
  T->snapmap = f->snapmap;
  T->nsnap = SIDE_META_NSNAP;
  T->nsnapmap = SIDE_META_NSNAPMAP;
  T->mcode = f->mcode;
  T->szmcode = SIDE_META_BODY_WORDS * (MSize)sizeof(MCode);
  T->mcloop = sizeof(MCode);
  fallback = f->mcode + SIDE_META_BODY_WORDS;
  gates = fallback + ARM64_EXIT_FALLBACK_WORDS;
  trace_exittab_rel(T, f->exittab);
  trace_exitstub_rel(T, gates);

  side_meta_setir(f->ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  side_meta_setir(f->ir, SIDE_META_R_VALUE, IR_SLOAD,
                  IRT_INT|IRT_GUARD, 2, IRSLOAD_TYPECHECK);
  for (i = 0; i < SIDE_META_NSNAP; i++) {
    MSize mapofs = i*SIDE_META_MAP_STRIDE;
    assert(footer_pc[i] < f->pt->sizebc);
    f->snap[i].mapofs = (uint32_t)mapofs;
    f->snap[i].ref = SIDE_META_R_VALUE;
    f->snap[i].nslots = f->pt->framesize;
    f->snap[i].topslot = f->pt->framesize;
    f->snap[i].nent = SIDE_META_NENT;
    f->snap[i].count = 0;
    f->snapmap[mapofs] = SNAP(2, 0, SIDE_META_R_VALUE);
    side_meta_pack_pc(&f->snapmap[mapofs+SIDE_META_NENT],
                      &bc[footer_pc[i]], 0);
    trace_exittarget_arm64_rel(G(L), T, (ExitNo)i, fallback);
  }
  /* The canonical live observation selects parent exit 2 and resumes at
  ** prototype bytecode offset 13. Keep those numbers explicit in this
  ** synthetic checkpoint even though no recorder is entered here. */
  side_meta_pack_pc(
    &f->snapmap[f->snap[SIDE_META_EXIT].mapofs+SIDE_META_NENT],
    &bc[SIDE_META_PC_POS], 0);

  proto_jit_startins_rel(f->pt, f->looppc, f->loopins);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));

  f->tracev.sizetrace = 2;
  setgcrefrel(f->tracev.slot[0], NULL);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));
  trace_sizetrace_rel(J, 2);
  tracevec_rel(J, (TraceVec *)&f->tracev);
}

static void side_meta_remove(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL);
  bc_publish(f->looppc, f->loopins);
  tracevec_rel(J, f->saved_tracev);
  trace_sizetrace_rel(J, f->saved_sizetrace);
  J->cur = f->saved_cur;
  J->parent = f->saved_parent;
  J->exitno = f->saved_exitno;
  J->pc = f->saved_pc;
  J->fn = f->saved_fn;
  J->pt = f->saved_pt;
  J->framedepth = f->saved_framedepth;
  J->retdepth = f->saved_retdepth;
  J->baseslot = f->saved_baseslot;
  copyTV(L, L->base-2, &f->saved_frame_function);
}

static int side_meta_check_at(lua_State *L, SideMetaFixture *f,
                              const BCIns *continuation, const BCIns *pc,
                              uint32_t context)
{
  uint8_t before = f->snap[SIDE_META_EXIT].count;
  int ok = lj_trace_test_arm64_first_side_loop_valid(
    L2J(L), context == LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA ? NULL : L,
    SIDE_META_PARENT, SIDE_META_EXIT, continuation, pc, context);
  assert(f->snap[SIDE_META_EXIT].count == before);
  return ok;
}

static int side_meta_check(lua_State *L, SideMetaFixture *f,
                           const BCIns *pc, uint32_t context)
{
  return side_meta_check_at(L, f, pc, pc, context);
}

static void expect_metadata_reject(lua_State *L, SideMetaFixture *f)
{
  assert(!side_meta_check(L, f, &proto_bc(f->pt)[SIDE_META_PC_POS],
                          LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
}

static void test_metadata_mutations(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  BCIns *bc = proto_bc(f->pt);
  SnapShot *selected = &f->snap[SIDE_META_EXIT];
  MSize footer = selected->mapofs+selected->nent;
  uint64_t saved_pcbase, bad_pcbase;
  SnapEntry saved_entry = f->snapmap[selected->mapofs];
  MCode *saved_mcode = T->mcode;
  MCode **saved_exittab = T->exittab;
  MCode *saved_exitstub = T->exitstub;
  void *saved_exittarget = la_loadptr_acq(
    (void *const *)&f->exittab[SIDE_META_EXIT]);
  MSize saved_szmcode = T->szmcode;

  assert(side_meta_check(L, f, &bc[SIDE_META_PC_POS],
                         LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, 0, SIDE_META_EXIT, &bc[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_NSNAP, &bc[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));

  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], NULL);
  expect_metadata_reject(L, f);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));

  trace_traceno_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_traceno_rel(T, SIDE_META_PARENT);
  T->retire_epoch = 1;
  expect_metadata_reject(L, f);
  T->retire_epoch = 0;
  T->unused1 |= TRACE_ENTRY_INVALIDATED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ENTRY_INVALIDATED;

  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_LOOP_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;
  T->unused1 |= TRACE_ARM64_INT_FORL_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_FORL_ADMITTED;
  T->unused1 |= TRACE_ARM64_INT_SIDE_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_SIDE_ADMITTED;

  T->root = SIDE_META_PARENT;
  expect_metadata_reject(L, f);
  T->root = 0;
  trace_link_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_link_rel(T, SIDE_META_PARENT);
  T->linktype = LJ_TRLINK_ROOT;
  expect_metadata_reject(L, f);
  T->linktype = LJ_TRLINK_LOOP;
  trace_nchild_rel(T, 1);
  expect_metadata_reject(L, f);
  trace_nchild_rel(T, 0);
  trace_nextside_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_nextside_rel(T, 0);

  trace_startpt_clear(T);
  expect_metadata_reject(L, f);
  trace_startpt_rel(T, f->pt);
  setmref(T->startpc, f->looppc+1);
  expect_metadata_reject(L, f);
  setmref(T->startpc, f->looppc);
  T->startins = BCINS_AJ(BC_LOOP, bc_a(f->loopins), 0);
  expect_metadata_reject(L, f);
  T->startins = f->loopins;

  bc_publish(f->looppc, f->loopins);
  expect_metadata_reject(L, f);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), 2));
  expect_metadata_reject(L, f);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));
  bc_publish(f->backpc, BCINS_AJ(BC_JMP, bc_a(f->backins), 0));
  expect_metadata_reject(L, f);
  bc_publish(f->backpc, f->backins);

  T->mcode = NULL;
  expect_metadata_reject(L, f);
  T->mcode = saved_mcode;
  trace_exittab_rel(T, NULL);
  expect_metadata_reject(L, f);
  trace_exittab_rel(T, saved_exittab);
  trace_exitstub_rel(T, NULL);
  expect_metadata_reject(L, f);
  trace_exitstub_rel(T, saved_exitstub);
  trace_exitstub_rel(T, saved_exitstub+1);
  expect_metadata_reject(L, f);
  trace_exitstub_rel(T, saved_exitstub);
  T->szmcode += 2u*sizeof(MCode);
  expect_metadata_reject(L, f);
  T->szmcode = saved_szmcode;
  T->unused1 |= TRACE_EXITTAB_MCODE;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_EXITTAB_MCODE;
  trace_exittarget_arm64_rel(G(L), T, SIDE_META_EXIT, T->mcode);
  expect_metadata_reject(L, f);
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT], saved_exittarget);
#if LJ_ABI_PAUTH
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT],
    trace_exittarget_arm64_encode((global_State *)(void *)T,
      exitstub_trace_fallback_addr_(saved_exitstub)));
  expect_metadata_reject(L, f);
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT], saved_exittarget);
#endif
  T->snap = NULL;
  expect_metadata_reject(L, f);
  T->snap = f->snap;
  T->snapmap = NULL;
  expect_metadata_reject(L, f);
  T->snapmap = f->snapmap;

  selected->count = SNAPCOUNT_DONE;
  expect_metadata_reject(L, f);
  selected->count = 0;
  selected->mapofs = SIDE_META_NSNAPMAP;
  expect_metadata_reject(L, f);
  selected->mapofs = SIDE_META_EXIT*SIDE_META_MAP_STRIDE;
  f->snap[SIDE_META_EXIT+1].mapofs++;
  expect_metadata_reject(L, f);
  f->snap[SIDE_META_EXIT+1].mapofs--;
  selected->nslots = 1;
  expect_metadata_reject(L, f);
  selected->nslots = f->pt->framesize;
  selected->topslot = (uint8_t)(f->pt->framesize+1u);
  expect_metadata_reject(L, f);
  selected->topslot = f->pt->framesize;
  f->snapmap[selected->mapofs] = SNAP(selected->nslots, 0,
                                      SIDE_META_R_VALUE);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry | SNAP_NORESTORE;
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry | UINT32_C(0x00800000);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = SNAP(2, 0, REF_DROP);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry;

#if LJ_FR2
  memcpy(&saved_pcbase, &f->snapmap[footer], sizeof(saved_pcbase));
  bad_pcbase = saved_pcbase | UINT64_C(1);
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  expect_metadata_reject(L, f);
  bad_pcbase = (uint64_t)(uintptr_t)&bc[SIDE_META_PC_POS-1] << 8;
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  expect_metadata_reject(L, f);
  bad_pcbase = (uint64_t)(uintptr_t)&bc[1] << 8;
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  assert(!side_meta_check(L, f, &bc[1],
                          LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  memcpy(&f->snapmap[footer], &saved_pcbase, sizeof(saved_pcbase));
#else
  (void)saved_pcbase;
  (void)bad_pcbase;
#endif
  assert(side_meta_check(L, f, &bc[SIDE_META_PC_POS],
                         LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
}

static void test_context_mutations(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  TGState *tg = L->tg_hint;
  BCIns *pc = &proto_bc(f->pt)[SIDE_META_PC_POS];
  TValue exact_frame;

  assert(tg != NULL && jit_token_acq(G(L)) == 0 &&
         lj_trace_state_load(J) == LJ_TRACE_IDLE && jit_owner_l_acq(J) == NULL);
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  copyTV(L, &exact_frame, L->base-2);
  copyTV(L, L->base-2, &f->saved_frame_function);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  copyTV(L, L->base-2, &exact_frame);

  lj_tg_poll_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_poll_rel(tg, 0);
  lj_tg_reqmask_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_reqmask_rel(tg, 0);
  lj_tg_profile_request_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_profile_request_rel(tg, 0);
  (void)lj_tg_hookmask_update(tg, 0, HOOK_VMEVENT);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  (void)lj_tg_hookmask_update(tg, HOOK_VMEVENT, 0);
  lj_tg_store_cur_L(tg, NULL);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_store_cur_L(tg, L);

  assert(lj_jit_token_try_l(L, J));
  lj_trace_state_store(J, LJ_TRACE_START);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  jit_owner_l_rel(J, L);
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_EXIT;
  J->pc = pc-1;
  J->fn = NULL;
  J->pt = NULL;
  J->framedepth = 7;
  J->retdepth = 9;
  J->baseslot = 31;
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc+1, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f,
                             &proto_bc(f->pt)[f->pt->sizebc], pc,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  J->parent = SIDE_META_PARENT+1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_EXIT-1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->exitno = SIDE_META_EXIT;
  jit_owner_l_rel(J, NULL);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  jit_owner_l_rel(J, L);

  memset(&J->cur, 0, sizeof(J->cur));
  trace_traceno_rel(&J->cur, 2);
  J->cur.root = SIDE_META_PARENT;
  trace_startpt_rel(&J->cur, f->pt);
  setmref(J->cur.startpc, pc);
  J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
  J->pc = pc-1;
  J->fn = f->fn;
  J->pt = f->pt;
  J->framedepth = 0;
  J->retdepth = 0;
  J->baseslot = (BCReg)(1+LJ_FR2);
  lj_trace_state_store(J, LJ_TRACE_RECORD);
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(side_meta_check_at(L, f, pc, pc+1,
                            LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc,
                             &proto_bc(f->pt)[f->pt->sizebc],
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  setmref(J->cur.startpc, pc+1);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  setmref(J->cur.startpc, pc);
  trace_traceno_rel(&J->cur, SIDE_META_PARENT);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  trace_traceno_rel(&J->cur, 2);
  J->cur.startins = BCINS_AD(BC_JMP, 1, 0);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
  J->baseslot++;
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->baseslot--;
  J->framedepth = 1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->framedepth = 0;
  J->retdepth = 1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->retdepth = 0;
  J->pt = NULL;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->pt = f->pt;
  J->cur.root = 0;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->cur.root = SIDE_META_PARENT;
  lj_trace_state_store(J, LJ_TRACE_RECORD_1ST);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  lj_trace_state_store(J, LJ_TRACE_ASM);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));

  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  SideMetaFixture fixture;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.off()\n"
    "function __arm64_side_meta_f(n, bias)\n"
    "  local i = 0\n"
    "  while i < n do\n"
    "    i = i + 1\n"
    "    if bias ~= 0 then i = i + 1 end\n"
    "  end\n"
    "  return i\n"
    "end\n");
  side_meta_install(L, &fixture);
  test_metadata_mutations(L, &fixture);
  test_context_mutations(L, &fixture);
  side_meta_remove(L, &fixture);
  lua_close(L);
  puts("t-arm64-jit-side-ingress-metadata OK: closed first-level LOOP metadata, snapshot and owner generations verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-side-ingress-metadata SKIP");
  return 0;
}

#endif
