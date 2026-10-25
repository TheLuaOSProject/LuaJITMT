/* Independent semantic and post-RA corruption checks for rooted operations.
** Positive sources are live native traces with different keys and tables. */
#include <assert.h>
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
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_target.h"
#include "lj_asm.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_trace.h"

static IRRef nth(const IRIns *ir, IRRef n, IROp op, MSize which)
{
  IRRef ref;
  for (ref = REF_FIRST; ref < n; ref++) {
    if (ir_load_acq(&ir[ref]).o == op && which-- == 0) return ref;
  }
  assert(!"missing rooted-read IR operation");
  return 0;
}

static int semantic_admit(jit_State *J, GCtrace *semantic)
{
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(J, semantic, &reject);
}

static int postra_admit(LJArm64PostRAView *view)
{
  IRRef nins = 0;
  return lj_asm_arm64_postra_admit(view, &nins);
}

int main(int argc, char **argv)
{
  lua_State *L;
  jit_State *J;
  GCtrace *owner, semantic;
  IRIns *ir, *semantic_good, *postra_ir;
  GCproto *owner_pt;
  SnapShot *snaps;
  SnapEntry *snapmap;
  IRRef nins, semantic_nins, tab0, key0, call0, call1, vload0, carg0;
  IRRef loopref, ref, guard0, zero0, payload_base = 0;
  SnapNo value_snap0 = 0, value_snap1 = 0, sn;
  MSize nrename = 0;
  jit_State *fake;
  LJArm64PostRAView view;
  assert(argc == 2 || argc == 3);
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  if (argc == 3) {
    lua_pushstring(L, argv[2]);
    lua_setglobal(L, "rooted_operations_admission");
  }
  assert(luaL_dostring(L, "package.path = \"./src/?.lua;./src/?/init.lua;\" .. package.path") == LUA_OK);
  if (luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "Lua fixture: %s\n", lua_tostring(L, -1));
    abort();
  }
  J = L2J(L);
  owner = traceref_safe(J, 1);
  assert(owner && trace_runnable_acq(owner, 1));
  owner_pt = gco2pt(trace_startptgco_acq(owner));
  nins = trace_nins_acq(owner);
  assert(owner->nk <= nins);
  for (ref = nins; ref-- > REF_FIRST; ) {
    if (trace_ir_acq(owner)[ref].o != IR_RENAME) break;
    nrename++;
  }
  semantic_nins = nins - nrename;
  ir = (IRIns *)calloc((size_t)nins, sizeof(IRIns));
  snaps = (SnapShot *)malloc(trace_nsnap_acq(owner) * sizeof(SnapShot));
  snapmap = (SnapEntry *)malloc(trace_nsnapmap_acq(owner) * sizeof(SnapEntry));
  assert(ir && snaps && snapmap);
  memcpy(ir + owner->nk, trace_ir_acq(owner) + owner->nk,
         (size_t)(nins - owner->nk) * sizeof(IRIns));
  for (ref = owner->nk; ref + 1 < REF_TRUE; ref++) {
    if (ir[ref].o == IR_KINT && ir[ref].t.irt == IRT_INT) {
      payload_base = ref;
      break;
    }
  }
  assert(payload_base != 0);
  memcpy(snaps, trace_snap_acq(owner), trace_nsnap_acq(owner) * sizeof(SnapShot));
  memcpy(snapmap, trace_snapmap_acq(owner),
         trace_nsnapmap_acq(owner) * sizeof(SnapEntry));

  semantic = *owner;
  semantic.ir = ir;
  semantic.snap = snaps;
  semantic.snapmap = snapmap;
  semantic.nins = semantic_nins;
  loopref = nth(ir, semantic_nins, IR_LOOP, 0);
  tab0 = nth(ir, semantic_nins, IR_TMPREF, 0);
  key0 = nth(ir, semantic_nins, IR_TMPREF, 1);
  call0 = nth(ir, semantic_nins, IR_CALLS, 0);
  call1 = nth(ir, semantic_nins, IR_CALLS, 1);
  guard0 = call0 + 1;
  zero0 = ir[guard0].op1 == call0 ? ir[guard0].op2 : ir[guard0].op1;
  vload0 = nth(ir, semantic_nins, IR_VLOAD, 0);
  carg0 = ir[ir[call0].op1].op1;
  for (sn = 0; sn < trace_nsnap_acq(owner); sn++) {
    if (snap_ref_acq(&snaps[sn]) == call0 + 2) value_snap0 = sn + 1;
    if (snap_ref_acq(&snaps[sn]) == call1 + 2) value_snap1 = sn + 1;
  }
  assert(value_snap0 && value_snap1);

  /* A compiled loop's published proto starts at JLOOP. Rebuild recorder
  ** context against an unmodified identical source proto, as the BC_LOOP
  ** bytecode certificate requires. */
  {
    fake = (jit_State *)calloc(1, sizeof(*fake));
    IRIns *semantic_ir = (IRIns *)calloc(nins, sizeof(*semantic_ir));
    SnapEntry *semantic_map = (SnapEntry *)malloc(
      trace_nsnapmap_acq(owner) * sizeof(*semantic_map));
    const BCIns *origpc = trace_startpc_acq(owner);
    GCproto *semantic_pt;
    BCIns *pc;
    SnapNo sn;
    assert(fake && semantic_ir && semantic_map && origpc);
    lua_getglobal(L, "rooted_read_trace_clone");
    assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top - 1)));
    semantic_pt = funcproto(funcV(L->top - 1));
    lua_pop(L, 1);
    pc = proto_bc(semantic_pt) + (origpc - proto_bc(owner_pt));
    assert(pc >= proto_bc(semantic_pt) &&
           pc < proto_bc(semantic_pt) + semantic_pt->sizebc &&
           (BCIns)la_load32_acq((const uint32_t *)pc) == owner->startins);
    memcpy(semantic_ir + owner->nk, ir + owner->nk,
           (nins - owner->nk) * sizeof(*semantic_ir));
    memcpy(semantic_map, snapmap,
           trace_nsnapmap_acq(owner) * sizeof(*semantic_map));
    for (sn = 0; sn < trace_nsnap_acq(owner); sn++) {
      SnapShot *snap = &snaps[sn];
      uint64_t pcbase;
      memcpy(&pcbase, &semantic_map[snap->mapofs + snap->nent], sizeof(pcbase));
      pcbase = ((((uint64_t)(uintptr_t)(proto_bc(semantic_pt) +
                 (((uintptr_t)(pcbase >> 8) -
                   (uintptr_t)proto_bc(owner_pt)) / sizeof(BCIns)))) << 8) |
                (uint8_t)pcbase);
      memcpy(&semantic_map[snap->mapofs + snap->nent], &pcbase, sizeof(pcbase));
    }
    semantic = *owner;
    semantic.ir = semantic_ir;
    semantic.snap = snaps;
    semantic.snapmap = semantic_map;
    trace_startpt_rel(&semantic, semantic_pt);
    setmref(semantic.startpc, pc);
    semantic.nins = nins;
    while (semantic.nins > REF_FIRST &&
           semantic_ir[semantic.nins - 1].o == IR_RENAME)
      semantic.nins--;
    for (ref = REF_FIRST; ref < semantic.nins; ref++) {
      if (semantic_ir[ref].o == IR_LOOP) {
        semantic_ir[ref].op1 = 0; semantic_ir[ref].op2 = 0;
      } else if (semantic_ir[ref].o == IR_XPOLL) {
        semantic_ir[ref].op1 = 1; semantic_ir[ref].op2 = 0;
      }
    }
    fake->L = L; fake->pt = semantic_pt; fake->startpc = pc;
    fake->baseslot = 1 + LJ_FR2;
    fake->loopref = loopref;
    J = fake;
    free(ir);
    ir = semantic_ir;
    semantic_good = (IRIns *)calloc((size_t)nins, sizeof(IRIns));
    postra_ir = (IRIns *)calloc((size_t)nins, sizeof(IRIns));
    assert(semantic_good && postra_ir);
    memcpy(semantic_good, semantic_ir, (size_t)nins * sizeof(IRIns));
    memcpy(postra_ir + owner->nk, trace_ir_acq(owner) + owner->nk,
           (size_t)(nins - owner->nk) * sizeof(IRIns));
    free(snapmap);
    snapmap = semantic_map;
  }
  assert(semantic_admit(J, &semantic));
#define SEM_REJECT(change) do { \
  memcpy(ir, semantic_good, (size_t)nins*sizeof(IRIns)); \
  semantic.ir = ir; semantic.nins = semantic_nins; change; \
  assert(!semantic_admit(J, &semantic)); \
} while (0)
  SEM_REJECT(ir[tab0].op2 = IRTMPREF_IN2);
  SEM_REJECT(ir[key0].op2 = IRTMPREF_IN1|IRTMPREF_OUT1);
  SEM_REJECT(ir[call0].op2 = IRCALL_lj_tab_gettv_rooted_hit_try);
  SEM_REJECT(ir[vload0].op2 = 8);
  SEM_REJECT(ir[vload0].o = IR_ALOAD);
  SEM_REJECT(ir[vload0].o = IR_HLOAD);
  SEM_REJECT(ir[carg0].op1 = ir[carg0].op2);
  SEM_REJECT(ir[call0].op1 = call0);
  SEM_REJECT(ir[call1].o = IR_NOP);
  SEM_REJECT(ir[guard0].o = IR_EQ);
  SEM_REJECT(ir[zero0].i = 1);
  SEM_REJECT(ir[key0].op1 = REF_NIL);
  SEM_REJECT(ir[carg0].op1 = owner->nk - 1);
  SEM_REJECT(ir[carg0].op1 = call1);
  SEM_REJECT(ir[carg0].op1 = (IRRef)nins);
  SEM_REJECT(ir[key0].op1 = owner->nk - 1);
  SEM_REJECT(ir[key0].op1 = key0);
  SEM_REJECT(ir[key0].op1 = (IRRef)nins);
  SEM_REJECT(do { IRRef kr = payload_base; \
    ir[kr].o = IR_KNUM; ir[kr].t.irt = IRT_NUM; ir[kr].op12 = 0; \
    ir[kr+1].o = IR_KINT; ir[kr+1].t.irt = IRT_INT; \
    ir[kr+1].i = 1; ir[key0].op1 = kr+1; } while (0));
  for (ref = owner->nk; ref < REF_TRUE; ref++) {
    if (semantic_good[ref].o == IR_KGC) {
      assert(semantic_good[ref].t.irt == IRT_STR);
      SEM_REJECT(ir[ref].t.irt = IRT_TAB);
      SEM_REJECT(setgcrefnull(ir[ref+LJ_GC64].gcr));
      SEM_REJECT(ir[key0].op1 = ref+LJ_GC64);
      ref += LJ_GC64;
    } else if (semantic_good[ref].o == IR_KNUM) ref++;
  }
  memcpy(ir, semantic_good, (size_t)nins * sizeof(IRIns));
  semantic.ir = ir; semantic.nins = semantic_nins;
  assert(semantic_admit(J, &semantic));
  snaps[value_snap0 - 1].ref = call0;
  assert(!semantic_admit(J, &semantic));
  snaps[value_snap0 - 1].ref = call0 + 2;
  assert(semantic_admit(J, &semantic));
  memcpy(ir, semantic_good, (size_t)nins * sizeof(IRIns));
  semantic.ir = ir; semantic.nins = semantic_nins;
  assert(semantic_admit(J, &semantic));
  snaps[value_snap1 - 1].ref = REF_DROP;
  assert(!semantic_admit(J, &semantic));
  snaps[value_snap1 - 1].ref = call1 + 2;
  assert(semantic_admit(J, &semantic));
#undef SEM_REJECT

  memset(&view, 0, sizeof(view));
  view.ir = postra_ir; view.owner = owner; view.snap = snaps; view.snapmap = snapmap;
  view.proto_bc = proto_bc(J->pt); view.nins = nins;
  view.nk = trace_nk_acq(owner); view.nsnap = trace_nsnap_acq(owner);
  view.nsnapmap = trace_nsnapmap_acq(owner);
  view.spadjust = trace_spadjust_acq(owner); view.proto_sizebc = J->pt->sizebc;
  view.proto_numparams = J->pt->numparams; view.root_topslot = trace_topslot_acq(owner);
  view.startins = trace_startins_acq(owner); view.base_delta = 0;
  assert(postra_admit(&view));
  tab0 = nth(postra_ir, nins, IR_TMPREF, 0);
  key0 = nth(postra_ir, nins, IR_TMPREF, 1);
  call0 = nth(postra_ir, nins, IR_CALLS, 0);
  call1 = nth(postra_ir, nins, IR_CALLS, 1);
  guard0 = call0 + 1;
  zero0 = postra_ir[guard0].op1 == call0 ? postra_ir[guard0].op2 :
                                            postra_ir[guard0].op1;
  vload0 = nth(postra_ir, nins, IR_VLOAD, 0);
  carg0 = postra_ir[postra_ir[call0].op1].op1;
#define POST_REJECT(change) do { \
  memcpy(postra_ir + owner->nk, trace_ir_acq(owner) + owner->nk, \
         (size_t)(nins - owner->nk)*sizeof(IRIns)); \
  view.ir = postra_ir; change; assert(!postra_admit(&view)); \
} while (0)
  POST_REJECT(postra_ir[tab0].op2 = IRTMPREF_IN2);
  POST_REJECT(postra_ir[key0].op2 = IRTMPREF_IN1|IRTMPREF_OUT1);
  POST_REJECT(postra_ir[call0].op2 = IRCALL_lj_tab_gettv_rooted_hit_try);
  POST_REJECT(postra_ir[vload0].op2 = 8);
  POST_REJECT(postra_ir[vload0].o = IR_ALOAD);
  POST_REJECT(postra_ir[vload0].o = IR_HLOAD);
  POST_REJECT(postra_ir[key0].r = postra_ir[tab0].r);
  POST_REJECT(postra_ir[carg0].op1 = postra_ir[carg0].op2);
  POST_REJECT(postra_ir[call0].op1 = call0);
  POST_REJECT(postra_ir[call1].o = IR_NOP);
  POST_REJECT(postra_ir[guard0].o = IR_EQ);
  POST_REJECT(postra_ir[zero0].i = 1);
  POST_REJECT(postra_ir[key0].op1 = REF_NIL);
  POST_REJECT(postra_ir[tab0].r = RID_INIT);
  POST_REJECT(do { postra_ir[tab0].r = RID_INIT; \
                   postra_ir[tab0].s = SPS_FIRST; } while (0));
  POST_REJECT(postra_ir[tab0].r = RID_D0);
  POST_REJECT(postra_ir[carg0].op1 = owner->nk - 1);
  POST_REJECT(postra_ir[carg0].op1 = call1);
  POST_REJECT(postra_ir[carg0].op1 = (IRRef)nins);
  POST_REJECT(postra_ir[key0].op1 = owner->nk - 1);
  POST_REJECT(postra_ir[key0].op1 = key0);
  POST_REJECT(postra_ir[key0].op1 = (IRRef)nins);
  POST_REJECT(do { IRRef kr = payload_base; \
    postra_ir[kr].o = IR_KNUM; postra_ir[kr].t.irt = IRT_NUM; \
    postra_ir[kr].op12 = 0; postra_ir[kr+1].o = IR_KINT; \
    postra_ir[kr+1].t.irt = IRT_INT; postra_ir[kr+1].i = 1; \
    postra_ir[key0].op1 = kr+1; } while (0));
  for (ref = owner->nk; ref < REF_TRUE; ref++) {
    IRIns k = trace_ir_acq(owner)[ref];
    if (k.o == IR_KGC) {
      assert(k.t.irt == IRT_STR);
      POST_REJECT(postra_ir[ref].t.irt = IRT_TAB);
      POST_REJECT(setgcrefnull(postra_ir[ref+LJ_GC64].gcr));
      POST_REJECT(postra_ir[key0].op1 = ref+LJ_GC64);
      ref += LJ_GC64;
    } else if (k.o == IR_KNUM) ref++;
  }
  memcpy(postra_ir + owner->nk, trace_ir_acq(owner) + owner->nk,
         (size_t)(nins - owner->nk) * sizeof(IRIns));
  view.ir = postra_ir;
  assert(postra_admit(&view));
  snaps[value_snap0 - 1].ref = call0;
  assert(!postra_admit(&view));
  snaps[value_snap0 - 1].ref = call0 + 2;
  assert(postra_admit(&view));
  memcpy(postra_ir + owner->nk, trace_ir_acq(owner) + owner->nk,
         (size_t)(nins - owner->nk) * sizeof(IRIns));
  view.ir = postra_ir;
  assert(postra_admit(&view));
  snaps[value_snap1 - 1].ref = REF_DROP;
  assert(!postra_admit(&view));
  snaps[value_snap1 - 1].ref = call1 + 2;
  assert(postra_admit(&view));
#undef POST_REJECT

  if (argc == 3 && strcmp(argv[2], "multiple") == 0) {
    unsigned spilled_tables = 0;
    /* Table pointers need two aligned spill words. Changing one span must
    ** fail independently of both scratch-address and numeric spill checks. */
    for (ref = REF_FIRST; ref < semantic_nins; ref++) {
      IRIns *slot = &postra_ir[ref];
      if (slot->o == IR_SLOAD && irt_type(slot->t) == IRT_TAB &&
          ra_hasspill(slot->s)) {
        uint8_t saved = slot->s;
        spilled_tables++;
        assert((saved & 1) == 0);
        slot->s = saved + 1;
        assert(!postra_admit(&view));
        slot->s = (uint8_t)(SPS_FIXED + view.spadjust / 4 - 1);
        assert(!postra_admit(&view));
        slot->s = saved;
        assert(postra_admit(&view));
      }
    }
    assert(spilled_tables > 0);
  }

  free(postra_ir); free(semantic_good); free(snapmap); free(snaps); free(ir);
  free(fake);
  lua_close(L);
  puts("t-arm64-jit-rooted-read-admission OK");
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-rooted-read-admission SKIP"); return 0; }
#endif
