/*
** ARM64 scalar LOOP capability: execute varied scalar dataflow, then verify
** independent recorder and immutable allocation certificates. The immutable
** IR copy ends at a guard page so malformed forward references cannot hide an
** out-of-bounds read. Semantic mutations use a separate copy and temporarily
** restore only this single-threaded test state's original LOOP bytecode.
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
#include <sys/mman.h>
#include <unistd.h>

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
  size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
  size_t irbytes = T->nins * sizeof(IRIns),
         mapsize = (irbytes + pagesize - 1) & ~(pagesize - 1);
  void *guarded = mmap(NULL, mapsize + pagesize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  IRIns *copy;
  assert(guarded != MAP_FAILED &&
         mprotect((char *)guarded + mapsize, pagesize, PROT_NONE) == 0);
  copy = (IRIns *)((char *)guarded + mapsize - irbytes);
  SnapEntry *map = malloc(T->nsnapmap * sizeof(*map));
  IRRef ref, loop = 0, num = 0, knum = 0, n;
  GCproto *pt = trace_startpt_acq(T);
  assert(copy && map);
  memcpy(copy + T->nk, T->ir + T->nk, (T->nins - T->nk) * sizeof(*copy));
  memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
  v.ir = copy;
  v.owner = T;
  v.snap = T->snap;
  v.snapmap = map;
  v.proto_bc = proto_bc(pt);
  v.nins = T->nins;
  v.nk = T->nk;
  v.nsnap = T->nsnap;
  v.nsnapmap = T->nsnapmap;
  v.spadjust = T->spadjust;
  v.proto_sizebc = pt->sizebc;
  v.proto_numparams = pt->numparams;
  v.root_topslot = T->topslot;
  v.startins = T->startins;
  assert(lj_asm_arm64_postra_admit(&v, &n));
  for (ref = REF_FIRST; ref < n; ref++) {
    if (copy[ref].o == IR_LOOP)
      loop = ref;
    if ((copy[ref].o == IR_ADD || copy[ref].o == IR_SUB) &&
        irt_isnum(copy[ref].t))
      num = ref;
  }
  for (ref = T->nk; ref < REF_TRUE; ref++)
    if (copy[ref].o == IR_KNUM) {
      knum = ref;
      break;
    }
  assert(loop && num);
  /* A future consumer has not yet been certified when an earlier absent
     location is examined. Its other operand must be checked before loading. */
  {
    IRRef mul = 0, use = 0;
    for (ref = REF_FIRST; ref + 1 < n; ref++)
      if (copy[ref].o == IR_MUL && irt_isnum(copy[ref].t)) {
        mul = ref;
        use = ref + 1;
        break;
      }
    if (mul) {
      copy[mul].t.irt = IRT_NUM;
      copy[mul].r = RID_INIT;
      copy[mul].s = 0;
      copy[use].o = IR_ADD;
      copy[use].t.irt = IRT_NUM;
      copy[use].op1 = mul;
      copy[use].op2 = T->nins;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mul] = T->ir[mul];
      copy[use] = T->ir[use];
    }
  }
  copy[num].o = IR_ALOAD;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[num] = T->ir[num];
  copy[num].o = IR_CALLS;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[num] = T->ir[num];
  copy[loop + 1].op1 = 0;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[loop + 1] = T->ir[loop + 1];
  copy[num].r = RID_GL;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[num] = T->ir[num];
  copy[num].s = SPS_FIRST + 1; /* NUM needs an aligned pair. */
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[num] = T->ir[num];
  copy[num].op1 = num;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[num] = T->ir[num];
  if (knum) {
    copy[num].op1 = knum + 1;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
  }

  {
    Reg reserved[] = {RID_GL, RID_DISPATCH, RID_FP, RID_SP};
    unsigned i;
    IRRef integer = 0;
    for (ref = REF_FIRST; ref < n; ref++)
      if (irt_isint(copy[ref].t) && copy[ref].o == IR_ADDOV) {
        integer = ref;
        break;
      }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
      copy[REF_BASE].r = reserved[i];
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[REF_BASE] = T->ir[REF_BASE];
      if (integer) {
        copy[integer].r = reserved[i];
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[integer] = T->ir[integer];
      }
    }
    copy[num].r = RID_SINK;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].r = RID_SUNK;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].r = RID_NONE | RID_GL;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].r = RID_INIT;
    copy[num].s = 0;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    if (integer) {
      copy[integer].s = 1;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[integer] = T->ir[integer];
    }
    if (copy[n].o == IR_RENAME) {
      copy[n].op1 = n;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
      copy[n].op2 = T->nsnap;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
      copy[n].r = RID_GL;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
    }
  }
  for (ref = REF_FIRST; ref < n; ref++) {
    if (copy[ref].o == IR_PHI && irt_isnum(copy[ref].t) &&
        !ra_hasspill(copy[ref].s) &&
        !ra_hasspill(copy[copy[ref].op1].s) &&
        !ra_hasspill(copy[copy[ref].op2].s)) {
      /* Unspilled PHIs preserve register correspondence. Dedicated numeric
      ** pressure tests cover saved inputs and the copy-spill path. */
      IRRef left = copy[ref].op1, right = copy[ref].op2;
      Reg changed = copy[ref].r == RID_D14 ? RID_D13 : RID_D14;
      copy[ref].r = changed;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[ref] = T->ir[ref];
      copy[left].r = changed;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[left] = T->ir[left];
      copy[right].r = copy[right].r == RID_D14 ? RID_D13 : RID_D14;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[right] = T->ir[right];
      break;
    }
  }
  v.spadjust += 16;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  v.spadjust = T->spadjust;
  map[0] = 0xffffffffu;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
  assert(lj_asm_arm64_postra_admit(&v, NULL));

  {
    jit_State *fake = calloc(1, sizeof(*fake));
    GCtrace semantic = *T;
    IRIns *semantic_ir = calloc(T->nins, sizeof(*semantic_ir));
    SnapEntry *semantic_map = malloc(T->nsnapmap * sizeof(*semantic_map));
    assert(semantic_ir && semantic_map);
    memcpy(semantic_ir + T->nk, T->ir + T->nk,
           (n - T->nk) * sizeof(*semantic_ir));
    memcpy(semantic_map, T->snapmap, T->nsnapmap * sizeof(*semantic_map));
    copy = semantic_ir;
    map = semantic_map;
    BCIns *pc = (BCIns *)(void *)trace_startpc_acq(T);
    BCIns saved = *pc;
    LJArm64IRReject reject;
    assert(fake);
    fake->L = L;
    fake->pt = pt;
    fake->startpc = pc;
    fake->baseslot = 1 + LJ_FR2;
    fake->loopref = loop;
    semantic.ir = copy;
    semantic.snapmap = map;
    semantic.nins = n;
    semantic.sinktags = 0;
    la_store32_rel((uint32_t *)pc, T->startins);
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num].r = RID_SINK;
    copy[num].s = 1;
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num] = T->ir[num];
    copy[REF_BASE].r = RID_GL;
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[REF_BASE] = T->ir[REF_BASE];
    copy[loop + 1].op1 = 0;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[loop + 1] = T->ir[loop + 1];
    copy[num].op1 = num;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num] = T->ir[num];
    copy[num].o = IR_ALOAD;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num] = T->ir[num];
    copy[num].o = IR_CALLS;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num] = T->ir[num];
    copy[num].t.irt |= IRT_GUARD;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[num] = T->ir[num];
    if (knum) {
      copy[num].op1 = knum + 1;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
    }
    fake->framedepth = 1;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    assert(lj_asm_arm64_postra_admit(&v, NULL));
    fake->framedepth = 0;
    semantic.link = 0;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic.link = T->link;
    map[0] = 0xffffffffu;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    la_store32_rel((uint32_t *)pc, saved);
    free(fake);
    free(semantic_ir);
    free(semantic_map);
    copy = (IRIns *)v.ir;
    map = (SnapEntry *)v.snapmap;
    puts("SEMANTIC poll, forward/payload refs, heap/call opcodes, flags, "
         "frame/link, snapshot rejected");
  }
  free(map);
  assert(munmap(guarded, mapsize + pagesize) == 0);
  (void)L;
  puts("MUTATIONS poll, register class, NUM spill alignment, forward/payload ref, "
       "frame, snapshot rejected");
}

static int reset(lua_State *L)
{
  (void)L;
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  return 0;
}
static int check(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef ref;
  unsigned ns = 0, nnum = 0, ntab = 0, ren = 0;
  assert(T && trace_runnable_acq(T, 1));
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
static int invoke(lua_State *L, int n)
{
  int i;
  lua_pushvalue(L, 1);
  for (i = 3; i <= n; i++)
    lua_pushvalue(L, i);
  return lua_pcall(L, n - 2, 1, 0);
}
static void equal_result(lua_State *L)
{
  if (lua_tonumber(L, -1) != lua_tonumber(L, 2))
    fprintf(stderr, "result %.17g expected %.17g\n", lua_tonumber(L, -1),
            lua_tonumber(L, 2));
  assert(lua_tonumber(L, -1) == lua_tonumber(L, 2));
  lua_pop(L, 1);
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

static int fma(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef ref;
  unsigned missing = 0, fused = 0, i;
  const MCode *mc = trace_mcode_acq(T);
  assert(T);
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_MUL && ra_noreg(T->ir[ref].r) && !T->ir[ref].s)
      missing++;
  for (i = 0; i < T->szmcode / sizeof(MCode); i++)
    if ((mc[i] & 0xffe08000u) == A64I_FMADDd ||
        (mc[i] & 0xffe08000u) == A64I_FMSUBd ||
        (mc[i] & 0xffe08000u) == A64I_FNMSUBd)
      fused++;
  assert(missing && fused);
  printf(
      "FMA absent multiplication locations=%u emitted fused operations=%u\n",
      missing, fused);
  return 0;
}
static int phis(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef r, loop = 0;
  unsigned num = 0, early = 0, duplicate = 0, renames = 0;
  assert(T && trace_runnable_acq(T, 1));
  for (r = REF_FIRST; r < T->nins; r++) {
    IRIns p = T->ir[r];
    if (p.o == IR_LOOP)
      loop = r;
    if (p.o == IR_RENAME)
      renames++;
    if (p.o == IR_PHI && irt_isnum(p.t)) {
      IRRef q;
      num++;
      early += p.op2 < loop;
      for (q = REF_FIRST; q < r; q++)
        if (T->ir[q].o == IR_PHI && T->ir[q].op2 == p.op2)
          duplicate++;
    }
  }
  printf("PHI numeric=%u earlier_rhs=%u duplicate_rhs=%u renames=%u\n",
         num, early, duplicate, renames);
  assert(num >= 2 && renames);
  assert(lua_toboolean(L, 1) ? (num == 2 && duplicate == 0) : early);
  return 0;
}
static int negative(lua_State *L)
{
  jit_State *J = L2J(L);
  TraceNo no;
  for (no = 1; no < trace_sizetrace_acq(J); no++)
    assert(!trace_runnable_acq(traceref_safe(J, no), no));
  assert(!lj_trace_test_root_entry_publishes());
  assert(!lj_trace_test_exit_calls());
  puts("NEGATIVE unsupported path stayed interpreted");
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
  lua_pushcfunction(L, fma);
  lua_setglobal(L, "probe_fma");
  lua_pushcfunction(L, phis);
  lua_setglobal(L, "probe_phis");
  lua_pushcfunction(L, negative);
  lua_setglobal(L, "probe_negative");
  assert(argc == 1 || argc == 2);
  if (luaL_dofile(L, argc == 2 ? argv[1]
                               : "tests/t-arm64-jit-scalar-capability.lua")) {
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
      "t-arm64-jit-scalar-capability SKIP: requires experimental macOS ARM64");
  return 0;
}
#endif
