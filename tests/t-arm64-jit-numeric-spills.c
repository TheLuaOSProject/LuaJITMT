/*
** ARM64 numeric spills: actual multi-result pressure loops,
** independent allocation corruptions and exact snapshot/lifecycle restoration.
*/
#include <stdio.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) &&     \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) &&                                     \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) &&                              \
    defined(LJ_TRACE_TEST_HELPERS)

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

static void mutate_layout(lua_State *L, GCtrace *T)
{
  LJArm64PostRAView v = {0};
  GCproto *pt = trace_startpt_acq(T);
  IRIns *copy = calloc(T->nins, sizeof(*copy));
  IRRef r, n, num = 0, num2 = 0, integer = 0, phi = 0, loop = 0;
  MSize capacity = SPS_FIXED + T->spadjust/4;
  unsigned spills = 0, spilledphis = 0, registerphis = 0;
  assert(copy);
  memcpy(copy+T->nk,T->ir+T->nk,(T->nins-T->nk)*sizeof(*copy));
  v.ir=copy;v.owner=T;v.snap=T->snap;v.snapmap=T->snapmap;
  v.proto_bc=proto_bc(pt);v.nins=T->nins;v.nk=T->nk;
  v.nsnap=T->nsnap;v.nsnapmap=T->nsnapmap;v.spadjust=T->spadjust;
  v.proto_sizebc=pt->sizebc;v.proto_numparams=pt->numparams;
  v.root_topslot=T->topslot;v.startins=T->startins;
  assert(lj_asm_arm64_postra_admit(&v,&n));
  for(r=REF_FIRST;r<n;r++) {
    IRIns p=copy[r];
    if(p.o==IR_LOOP)loop=r;
    if(irt_isnum(p.t)&&ra_hasspill(p.s))spills++;
    if(p.o==IR_SLOAD&&irt_isnum(p.t)&&ra_hasspill(p.s)) {
      if(!num)num=r; else if(!num2)num2=r;
    }
    if(p.o==IR_ADDOV&&irt_isint(p.t))integer=r;
    if(p.o==IR_PHI&&irt_isnum(p.t)) {
      if(ra_hasspill(p.s)) {
        IRIns left=copy[p.op1],right=copy[p.op2];
        assert(p.r==RID_INIT && right.s==p.s && ra_hasspill(left.s));
        assert(left.s!=p.s); /* Exercise the actual copy-spill path. */
        if(!phi)phi=r;
        spilledphis++;
      } else if(ra_hasspill(copy[p.op1].s))registerphis++;
    }
  }
  assert(spills && num && integer && loop);
  if (lua_toboolean(L, 2))
    assert(spilledphis && registerphis && num2);
  printf("SPILL proof numeric_entries=%u spilled_phis=%u register_phi_left_spills=%u frame=%u\n",
    spills,spilledphis,registerphis,T->spadjust);

  copy[num].s|=1;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  copy[num]=T->ir[num];
  copy[num].s=1;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  copy[num]=T->ir[num];
  copy[num].s=(uint8_t)capacity;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  copy[num]=T->ir[num];
  /* Last legal double slot, then the adjacent odd/out-of-range pair. */
  copy[num].s=SPS_LIMIT-2;
  v.spadjust=sps_scale(SPS_LIMIT-SPS_FIXED);
  assert(lj_asm_arm64_postra_admit(&v,NULL));
  copy[num].s=SPS_LIMIT-1;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  copy[num]=T->ir[num];v.spadjust=T->spadjust;
  if(num2) {
    copy[num2].s=copy[num].s;
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[num2]=T->ir[num2];
  }
  copy[integer].s=copy[num].s+1;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  copy[integer]=T->ir[integer];
  v.spadjust+=16;
  assert(!lj_asm_arm64_postra_admit(&v,NULL));
  v.spadjust=T->spadjust;
  if(v.spadjust) {
    v.spadjust-=16;
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    v.spadjust=T->spadjust;
  }
  if(phi) {
    IRRef left=copy[phi].op1,right=copy[phi].op2;
    copy[phi].r=RID_D8;
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[phi]=T->ir[phi];
    assert(capacity+2<SPS_LIMIT);
    copy[phi].s=(uint8_t)capacity;
    v.spadjust=sps_scale(sps_align(capacity+2));
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[phi]=T->ir[phi];v.spadjust=T->spadjust;
    copy[right].s=(uint8_t)capacity;
    v.spadjust=sps_scale(sps_align(capacity+2));
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[right]=T->ir[right];v.spadjust=T->spadjust;
    copy[left].s=0;
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[left]=T->ir[left];
  }
  /* The recorder certificate remains independent of allocator locations. */
  {
    GCtrace semantic=*T;
    jit_State *fake=calloc(1,sizeof(*fake));
    BCIns *pc=(BCIns *)(void *)trace_startpc_acq(T);
    BCIns saved=*pc;
    LJArm64IRReject reject;
    assert(fake);
    fake->L=L;fake->pt=pt;fake->startpc=pc;
    fake->baseslot=1+LJ_FR2;fake->loopref=loop;
    semantic.ir=copy;semantic.nins=n;semantic.sinktags=0;
    copy[num].s=255;
    la_store32_rel((uint32_t *)pc,T->startins);
    assert(lj_asm_arm64_ir_admit(fake,&semantic,&reject));
    la_store32_rel((uint32_t *)pc,saved);
    assert(!lj_asm_arm64_postra_admit(&v,NULL));
    copy[num]=T->ir[num];
    free(fake);
  }
  assert(lj_asm_arm64_postra_admit(&v,NULL));
  free(copy);
  puts("SPILL mutations: width, alignment, extent, overlap, frame and PHI correspondence rejected");
}

static int reset(lua_State *L)
{
  (void)L;
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  return 0;
}
/* Match a double load/store to a particular stack slot and register,
** including the emitter's paired form. Offsets are byte offsets from SP. */
static int spill_instruction(MCode ins, int load, Reg reg, MSize slot)
{
  MCode single = load ? A64I_LDRd : A64I_STRd;
  MCode pair = load ? A64I_LDPd : A64I_STPd;
  int32_t offset = (int32_t)sps_scale(slot);
  reg &= 31;
  if ((ins & 0xffc003e0u) == (single | (RID_SP << 5)))
    return (ins & 31) == reg && (int32_t)((ins >> 10) & 4095)*8 == offset;
  if ((ins & 0xffc003e0u) == (pair | (RID_SP << 5))) {
    int32_t pair_offset = (int32_t)((ins >> 15) & 127);
    if (pair_offset & 64) pair_offset -= 128;
    pair_offset *= 8;
    return ((ins & 31) == reg && pair_offset == offset) ||
           (((ins >> 10) & 31) == reg && pair_offset + 8 == offset);
  }
  return 0;
}

static int check(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef ref;
  unsigned ns = 0, nnum = 0, ntab = 0, ren = 0;
  int expected_frame = (int)luaL_checkinteger(L, 1);
  assert(T && trace_runnable_acq(T, 1));
  if (expected_frame >= 0)
    assert(T->spadjust == (MSize)expected_frame);
  else
    assert(T->spadjust > 0);
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_RENAME)
      ren++;
    if (ra_hasspill(ir->s)) {
      ns++;
      if (irt_isnum(ir->t))
        nnum++;
      if (irt_istab(ir->t))
        ntab++;
    }
  }
  printf("NATIVE publish=%u exits=%u last_exit=%u spadjust=%u spills=%u "
         "numeric=%u tab=%u rename=%u\n",
         lj_trace_test_root_entry_publishes(), lj_trace_test_exit_calls(),
         lj_trace_test_last_exitno(), T->spadjust, ns, nnum, ntab, ren);
  assert(lj_trace_test_root_entry_publishes() > 0 &&
         lj_trace_test_exit_calls() > 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  if (lua_toboolean(L, 3)) {
    const MCode *mc = trace_mcode_acq(T);
    unsigned fused = 0, spilled_rhs = 0, i;
    for (i = 0; i < T->szmcode / sizeof(MCode); i++)
      if ((mc[i] & 0xffe08000u) == A64I_FMADDd ||
          (mc[i] & 0xffe08000u) == A64I_FMSUBd ||
          (mc[i] & 0xffe08000u) == A64I_FNMSUBd)
        fused++;
    for (ref = REF_FIRST; ref < T->nins; ref++) {
      IRIns phi = T->ir[ref];
      if (phi.o == IR_PHI && ra_hasreg(phi.r)) {
        IRIns right = T->ir[phi.op2];
        if (irt_isnum(right.t) && ra_hasspill(right.s) && right.r != phi.r) {
          unsigned saved = 0, restored = 0;
          for (i = 0; i < T->szmcode / sizeof(MCode); i++) {
            if (spill_instruction(mc[i], 0, right.r, right.s)) saved = 1;
            if (saved && spill_instruction(mc[i], 1, phi.r, right.s))
              restored = 1;
          }
          assert(saved && restored);
          spilled_rhs++;
        }
      }
    }
    assert(fused && spilled_rhs);
    printf("FMA fused instructions=%u spilled PHI inputs reloaded=%u\n",
           fused, spilled_rhs);
  }
  mutate_layout(L, T);
  return 0;
}
typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ | TGF_STOPREQ_FRESH));
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
        LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&publisher->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(publisher->g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(publisher->tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(publisher->g) == 0);
  assert(lj_tg_reqmask_acq(publisher->tg) == 0);
  assert(lj_tg_poll_acq(publisher->tg) == 0);
  if (lj_tg_load_jit_base(publisher->tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(publisher->tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    gc2_hs_actions_rel(publisher->g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(publisher->g, 1);
    gc2_hs_epoch_rel(publisher->g, publisher->epoch + 1u);
    lj_tg_reqmask_rel(publisher->tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(publisher->tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void native_idle(lua_State *L, int32_t vmstate)
{
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == vmstate);
}
static int result_base;
static int invoke(lua_State *L, int n)
{
  int i;
  result_base = n;
  lua_pushvalue(L, 1);
  for (i = 3; i <= n; i++)
    lua_pushvalue(L, i);
  return lua_pcall(L, n - 2, LUA_MULTRET, 0);
}
static void equal_result(lua_State *L)
{
  int i, count=(int)lua_objlen(L,2);
  assert(count && lua_gettop(L)==result_base+count);
  for (i=1;i<=count;i++) {
    lua_rawgeti(L,2,i);
    if(lua_tonumber(L,result_base+i)!=lua_tonumber(L,-1))
      fprintf(stderr,"result %d %.17g expected %.17g\n",i,
        lua_tonumber(L,result_base+i),lua_tonumber(L,-1));
    assert(lua_tonumber(L,result_base+i)==lua_tonumber(L,-1));
    lua_pop(L,1);
  }
  lua_settop(L,result_base);
}

static int lifecycle(lua_State *L)
{
  int n = lua_gettop(L), status;
  GCtrace *T = traceref_safe(L2J(L), 1);
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  int32_t vmstate = lj_tg_vmstate_load_acq(tg);
  IRRef ref, loop = 0;
  SnapNo s, loopsnap = (SnapNo)~0u, finalexit;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  assert(T && trace_runnable_acq(T, 1));
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_LOOP)
      loop = ref;
  for (s = 0; s < T->nsnap; s++)
    if (T->snap[s].ref == loop)
      loopsnap = s;
  assert(loop && loopsnap != (SnapNo)~0u);
  finalexit = T->nsnap - 1;

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher =
      (PostAdmissionPublisher){g, tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
                        &publisher) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == 0);
  equal_result(L);
  assert(publisher.published && lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_exit_calls() == 2 &&
         lj_trace_test_first_exitno() == loopsnap);
  assert(lj_trace_test_last_exitno() == finalexit &&
         lj_tg_profile_request_acq(tg) == 0);
  native_idle(L, vmstate);

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher =
      (PostAdmissionPublisher){g, tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
                        &publisher) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown"));
  lua_pop(L, 1);
  assert(publisher.published && lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1 &&
         lj_trace_test_last_exitno() == loopsnap);
  assert(gc2_hs_epoch_acq(g) == epoch + 1 &&
         lj_tg_hs_epoch_ack_acq(tg) == epoch + 1);
  assert(!gc2_hs_pending_acq(g) && !lj_tg_reqmask_acq(tg) &&
         !lj_tg_poll_acq(tg));
  assert(lj_tg_flags_acq(tg) & TGF_STOPREQ);
  native_idle(L, vmstate);
  clear_stopreq(tg);

  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_last_exitno() == finalexit);
  native_idle(L, vmstate);
  lua_gc(L, LUA_GCCOLLECT, 0);
  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1);
  native_idle(L, vmstate);
  puts("LIFECYCLE post-admission profile, STOPREQ unwind, resumed entry, full "
       "GC passed");
  return 0;
}
static int changed(lua_State *L)
{
  int n = lua_gettop(L);
  int32_t vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  GCtrace *T = traceref_safe(L2J(L), 1);
  assert(T);
  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() > 0 &&
         lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_first_exitno() < T->nsnap - 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  native_idle(L, vmstate);
  printf("CHANGED input guard exit %u, numeric snapshot restored\n",
         lj_trace_test_first_exitno());
  return 0;
}
static int flush(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  GCproto *pt;
  const BCIns *pc;
  BCIns ins;
  assert(T);
  pt = trace_startpt_acq(T);
  pc = trace_startpc_acq(T);
  ins = T->startins;
  assert(lj_trace_flushall_gc(L) == 0);
  assert(la_load32_acq((const uint32_t *)pc) == ins && !proto_trace_acq(pt));
  T = traceref_safe(L2J(L), 1);
  assert(!T || !trace_runnable_acq(T, 1));
  puts("FLUSH root bytecode restored and native trace retired");
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, reset);
  lua_setglobal(L, "probe_reset");
  lua_pushcfunction(L, check);
  lua_setglobal(L, "probe_check");
  lua_pushcfunction(L, lifecycle);
  lua_setglobal(L, "probe_lifecycle");
  lua_pushcfunction(L, changed);
  lua_setglobal(L, "probe_changed");
  lua_pushcfunction(L, flush);
  lua_setglobal(L, "probe_flush");
  assert(argc == 1 || argc == 2);
  if (luaL_dofile(L, argc == 2 ? argv[1]
                               : "tests/t-arm64-jit-numeric-spills.lua")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);
  return 0;
}

#else
int main(void)
{
  puts(
      "t-arm64-jit-numeric-spills SKIP: requires experimental macOS ARM64");
  return 0;
}
#endif
