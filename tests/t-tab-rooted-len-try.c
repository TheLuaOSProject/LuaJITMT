/*
** Focused bounded authoritative-root table length tests.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_target.h"
#if LJ_TARGET_ARM64
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_thr.h"
#endif

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-rooted-len-try requires LJ_TAB_TEST_HELPERS"
#endif
#ifndef LJ_GC2_TEST_HELPERS
#error "t-tab-rooted-len-try requires LJ_GC2_TEST_HELPERS"
#endif

typedef struct CleanState {
  uint32_t readers;
  uint32_t anchors;
  uint32_t tab_read_depth;
  uint64_t tab_read_epoch;
} CleanState;

typedef struct WaitState {
  uint32_t no_l;
  uint32_t l;
  uint32_t store_l;
} WaitState;

typedef struct LeaseState {
  GCArena *arena;
  uint64_t remote_active;
  uint32_t cell;
  uint32_t lifetime;
} LeaseState;

enum {
  LEN_HOOK_NONE,
  LEN_HOOK_OBSERVE,
  LEN_HOOK_GENERATION,
  LEN_HOOK_ROOT,
  LEN_HOOK_OWNER
};

typedef struct LenHookState {
  lua_State *L;
  GCtab *table;
  TValue *root;
  TValue *array;
  uint64_t replacement;
  LJStateOwner owner;
  uint32_t action;
  uint32_t hits;
} LenHookState;

static LenHookState len_hook;

static CleanState clean_state(lua_State *L)
{
  CleanState state;
  TGState *tg = L2TG(L);
  state.readers = gc2_smr_readers_acq(G(L));
  state.anchors = lj_tg_root_anchor_top_acq(tg);
  state.tab_read_depth = lj_tg_tab_read_depth_acq(tg);
  state.tab_read_epoch = lj_tg_tab_read_epoch_acq(tg);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return state;
}

static void assert_clean(lua_State *L, CleanState state)
{
  TGState *tg = L2TG(L);
  assert(gc2_smr_readers_acq(G(L)) == state.readers);
  assert(lj_tg_root_anchor_top_acq(tg) == state.anchors);
  assert(lj_tg_tab_read_depth_acq(tg) == state.tab_read_depth);
  assert(lj_tg_tab_read_epoch_acq(tg) == state.tab_read_epoch);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static WaitState wait_state(void)
{
  WaitState state;
  state.no_l = lj_tab_test_wait_no_l_calls();
  state.l = lj_tab_test_wait_l_calls();
  state.store_l = lj_tab_test_store_wait_l_calls();
  return state;
}

static void assert_wait_state(WaitState state)
{
  assert(lj_tab_test_wait_no_l_calls() == state.no_l);
  assert(lj_tab_test_wait_l_calls() == state.l);
  assert(lj_tab_test_store_wait_l_calls() == state.store_l);
}

static LeaseState lease_state(GCtab *t)
{
  LeaseState state;
  uint32_t flags;
  state.arena = lj_arena_of(t);
  flags = lj_arena_flags_acq(state.arena);
  assert((flags & LJ_AF_HUGE_MAGIC) != LJ_AF_HUGE_MAGIC);
  assert((flags & LJ_AF_TRAVERSABLE) != 0);
  state.cell = lj_arena_cellof(t);
  state.remote_active = lj_arena_remote_active_acq(state.arena);
  state.lifetime = lj_arena_lifetime_state_acq(state.arena, state.cell);
  return state;
}

static void assert_lease_state(GCtab *t, LeaseState state)
{
  assert(lj_arena_of(t) == state.arena);
  assert(lj_arena_cellof(t) == state.cell);
  assert(lj_arena_remote_active_acq(state.arena) == state.remote_active);
  assert(lj_arena_lifetime_state_acq(state.arena, state.cell) ==
	 state.lifetime);
}

static int32_t bounded_len(lua_State *L, cTValue *tabroot, GCtab *t)
{
  CleanState clean = clean_state(L);
  WaitState waits = wait_state();
  LeaseState lease = lease_state(t);
  GCSize total = lj_gc_total_load(G(L));
  int32_t len = lj_tab_len_rooted_try(L, tabroot);
  assert(lj_gc_total_load(G(L)) == total);
  assert_lease_state(t, lease);
  assert_wait_state(waits);
  assert_clean(L, clean);
  return len;
}

static int32_t bounded_forjit_len(lua_State *L, cTValue *tabroot, GCtab *t)
{
  CleanState clean = clean_state(L);
  WaitState waits = wait_state();
  LeaseState lease = lease_state(t);
  GCSize total = lj_gc_total_load(G(L));
  int32_t len = lj_tab_len_forjit_try(L, tabroot);
  assert(lj_gc_total_load(G(L)) == total);
  assert_lease_state(t, lease);
  assert_wait_state(waits);
  assert_clean(L, clean);
  return len;
}

static void push_dense_table(lua_State *L, int32_t len)
{
  int32_t i;
  /* Stay above the colocated-array ceiling so structural retry tests can
  ** republish a retired separated generation. */
  lua_createtable(L, len + LJ_MAX_COLOSIZE + 1, 0);
  for (i = 1; i <= len; i++) {
    lua_pushinteger(L, i * 11);
    lua_rawseti(L, -2, i);
  }
}

static void len_try_hook(GCtab *t)
{
  assert(len_hook.action != LEN_HOOK_NONE);
  assert(t == len_hook.table);
  len_hook.hits++;
  if (len_hook.action == LEN_HOOK_OBSERVE) {
    /* Only the generated ABI can reach this hook without first entering the
    ** general SMR/table-lease helper. */
#if LJ_TARGET_ARM64
    assert(lj_tab_test_len_rooted_try_calls() > 0u);
#else
    assert(lj_tab_test_len_rooted_try_calls() == 0u);
#endif
  } else if (len_hook.action == LEN_HOOK_GENERATION) {
    assert(len_hook.array != NULL);
    lj_tab_array_rel(t, len_hook.array);
  } else if (len_hook.action == LEN_HOOK_ROOT) {
    assert(len_hook.root != NULL);
    tv_rawstore_rel(len_hook.root, len_hook.replacement);
  } else {
    assert(len_hook.action == LEN_HOOK_OWNER && len_hook.L != NULL);
    lj_state_owner_word_rel(len_hook.L, 0);
  }
}

static void arm_len_hook(uint32_t action, lua_State *L, GCtab *t,
			 TValue *root, TValue *array, uint64_t replacement)
{
  memset(&len_hook, 0, sizeof(len_hook));
  len_hook.action = action;
  len_hook.L = L;
  len_hook.table = t;
  len_hook.root = root;
  len_hook.array = array;
  len_hook.replacement = replacement;
  if (L)
    len_hook.owner = lj_state_owner_word_acq(L);
  lj_tab_test_set_len_rooted_try_hook(len_try_hook);
}

static void exercise_forjit_direct_rejections(lua_State *L, lua_State *wrong)
{
  int top = lua_gettop(L);
  CleanState clean;
  WaitState waits;
  GCSize total;
  GCtab *t;
  cTValue *tabroot;

  /* This ABI is only admitted from generated code while its TG owns L.
  ** Direct C calls must remain bounded rejections; do not forge JIT state. */
  push_dense_table(L, 4);
  tabroot = L->top - 1;
  t = tabV(tabroot);
  assert(bounded_forjit_len(L, tabroot, t) == LJ_TAB_LEN_RETRY);
  assert(bounded_forjit_len(wrong, tabroot, t) == LJ_TAB_LEN_RETRY);

  lua_pushinteger(L, 17);
  clean = clean_state(L);
  waits = wait_state();
  total = lj_gc_total_load(G(L));
  assert(lj_tab_len_forjit_try(L, L->top - 1) == LJ_TAB_LEN_RETRY);
  assert(lj_tab_len_forjit_try(L, NULL) == LJ_TAB_LEN_RETRY);
  assert(lj_tab_len_forjit_try(NULL, tabroot) == LJ_TAB_LEN_RETRY);
  assert(lj_gc_total_load(G(L)) == total);
  assert_wait_state(waits);
  assert_clean(L, clean);

  lua_settop(L, top);
}

static void exercise_success(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  uint64_t root;

  lua_newtable(L);
  t = tabV(L->top - 1);
  assert(bounded_len(L, L->top - 1, t) == 0);
  lua_pop(L, 1);

  push_dense_table(L, 9);
  t = tabV(L->top - 1);
  root = tv_rawload(L->top - 1);
  assert(bounded_len(L, L->top - 1, t) == 9);
  assert(tv_rawload(L->top - 1) == root);
  lua_settop(L, top);
}

static void exercise_admission_retries(lua_State *L)
{
  int top = lua_gettop(L);
  global_State *g = G(L);
  GCtab *t;
  uint32_t expect;

  push_dense_table(L, 7);
  t = tabV(L->top - 1);

  expect = LJ_GC2_SMR_OPEN;
  assert(gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(bounded_len(L, L->top - 1, t) == LJ_TAB_LEN_RETRY);
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);

  lj_gc2_test_stack_admission_retry_once(obj2gco(t));
  assert(bounded_len(L, L->top - 1, t) == LJ_TAB_LEN_RETRY);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert(bounded_len(L, L->top - 1, t) == 7);

  lua_settop(L, top);
}

static void exercise_structural_retries(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, hmask;
  uint32_t hbits;

  push_dense_table(L, 7);
  t = tabV(L->top - 1);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldarray && !lj_tab_array_is_colocated(t, oldarray));
  hmask = lj_tab_node_hmask_acq(lj_tab_node_acq(t));
  hbits = hmask ? lj_fls(hmask) + 1u : 0u;
  lj_tab_resize(L, t, (uint32_t)oldasize + 32u, hbits);
  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray && lj_tab_array_is_retiring(t, oldarray));

  /* A captured root which already names RETIRING storage fails immediately. */
  lj_tab_array_rel(t, oldarray);
  assert(bounded_len(L, L->top - 1, t) == LJ_TAB_LEN_RETRY);
  lj_tab_array_rel(t, newarray);
  assert(bounded_len(L, L->top - 1, t) == 7);

  /* Changing the root after capture but before paired-current validation is
  ** likewise a bounded generation retry. */
  arm_len_hook(LEN_HOOK_GENERATION, L, t, NULL, oldarray, 0);
  assert(bounded_len(L, L->top - 1, t) == LJ_TAB_LEN_RETRY);
  assert(len_hook.hits == 1u && lj_tab_array_acq(t) == oldarray);
  lj_tab_array_rel(t, newarray);
  assert(bounded_len(L, L->top - 1, t) == 7);

  lua_settop(L, top);
}

static void exercise_root_and_owner_validation(lua_State *L, lua_State *wrong)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *tabroot, *replacement;
  uint64_t saved;

  push_dense_table(L, 5);  /* Stable independent root for the source table. */
  t = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_newtable(L);
  tabroot = L->top - 2;
  replacement = L->top - 1;
  saved = tv_rawload(tabroot);

  arm_len_hook(LEN_HOOK_ROOT, L, t, tabroot, NULL,
	       tv_rawload(replacement));
  assert(bounded_len(L, tabroot, t) == LJ_TAB_LEN_RETRY);
  assert(len_hook.hits == 1u &&
	 tv_rawload(tabroot) == tv_rawload(replacement));
  tv_rawstore_rel(tabroot, saved);
  assert(bounded_len(L, tabroot, t) == 5);

  arm_len_hook(LEN_HOOK_OWNER, L, t, NULL, NULL, 0);
  assert(bounded_len(L, tabroot, t) == LJ_TAB_LEN_RETRY);
  assert(len_hook.hits == 1u && lj_state_owner_word_acq(L) == 0);
  lj_state_owner_word_rel(L, len_hook.owner);
  assert(bounded_len(L, tabroot, t) == 5);

  /* A state which this physical actor does not own never admits SMR or a
  ** table lease, even if handed another state's live root address. */
  assert(bounded_len(wrong, tabroot, t) == LJ_TAB_LEN_RETRY);

  lua_settop(L, top);
}

static void run_script(lua_State *L, const char *script)
{
  int status = luaL_dostring(L, script);
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "embedded Lua script failed");
  }
  assert(status == 0);
}

#if LJ_TARGET_ARM64
typedef struct RootedLenPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  uint32_t saw_stage, saw_jit_base, published;
} RootedLenPublisher;

static void *publish_rooted_len_stopreq(void *arg)
{
  RootedLenPublisher *p = (RootedLenPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&p->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&p->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(p->g) == p->epoch);
  assert(lj_tg_hs_epoch_ack_acq(p->tg) == p->epoch);
  assert(gc2_hs_pending_acq(p->g) == 0);
  assert(lj_tg_reqmask_acq(p->tg) == 0 && lj_tg_poll_acq(p->tg) == 0);
  if (lj_tg_load_jit_base(p->tg) != NULL)
    la_store32_rel(&p->saw_jit_base, 1);
  assert(la_load32_acq(&p->saw_jit_base) == 1);
  gc2_hs_actions_rel(p->g, LJ_GC2_HS_STOPREQ);
  gc2_hs_pending_rel(p->g, 1);
  gc2_hs_epoch_rel(p->g, p->epoch + 1u);
  lj_tg_reqmask_rel(p->tg, LJ_GC2_HS_STOPREQ);
  lj_tg_poll_rel(p->tg, 1);
  la_store32_rel(&p->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ | TGF_STOPREQ_FRESH));
}

static int call_rooted_len(lua_State *L)
{
  lua_getglobal(L, "rooted_len_trace_call");
  lua_getglobal(L, "rooted_len_trace_table");
  lua_pushinteger(L, 80);
  return lua_pcall(L, 2, 1, 0);
}

static void assert_rooted_len_result(lua_State *L)
{
  if (call_rooted_len(L) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "rooted length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
}

static void assert_rooted_len_native(lua_State *L, const char *stage)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t successes = lj_trace_test_root_entry_successes();
  uint32_t exits = lj_trace_test_exit_calls();
  uint32_t helpers = lj_tab_test_len_rooted_try_calls();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  uint32_t startins = lj_trace_test_root_entry_startins_calls();
  if (!publishes || !exits || !helpers) {
    GCproto *pt;
    fprintf(stderr, "rooted length %s: publishes=%u successes=%u exits=%u "
      "helpers=%u cleanups=%u startins=%u phase=%u gate=%u open=%d "
      "req=%u poll=%u flags=%u vmstate=%d native=%u jit_base=%p "
      "jit_active=%d smr_readers=%u anchors=%u tab_read_depth=%u "
      "rootdesc=%u\n", stage, publishes, successes, exits, helpers,
      cleanups, startins, gc2_phase_acq(g),
      gc2_jit_phase_gate_acq(g), lj_gc2_jit_entry_open(g),
      lj_tg_reqmask_acq(tg), lj_tg_poll_acq(tg), lj_tg_flags_acq(tg),
      lj_tg_vmstate_load_acq(tg), lj_tg_in_native_acq(tg),
      (void *)lj_tg_load_jit_base(tg), lj_tg_jit_active_acq(tg),
      gc2_smr_readers_acq(g), lj_tg_root_anchor_top_acq(tg),
      lj_tg_tab_read_depth_acq(tg),
      (unsigned)lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL));
    lua_getglobal(L, "rooted_len_trace_call");
    pt = funcproto(funcV(L->top-1));
    for (MSize pc = 0; pc < pt->sizebc; pc++) {
      BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pc]);
      if (!pc || bc_op(ins) == BC_LOOP || bc_op(ins) == BC_ILOOP ||
          bc_op(ins) == BC_JLOOP)
        fprintf(stderr, "rooted length source pc=%u ins=%08x flags=%u\n",
                pc, ins, la_load8_acq(&pt->flags));
    }
    lua_pop(L, 1);
  }
  assert(publishes > 0 && exits > 0 && helpers > 0);
}
#endif

#if LJ_TARGET_ARM64
static const void *retry_record_func;
static GCproto *retry_record_pt;
static uint32_t retry_record_len_calls;

static int record_retry_len(lua_State *L)
{
  if (lua_topointer(L, 2) == retry_record_func) {
    lua_Integer pc = lua_tointeger(L, 3);
    assert(pc >= 0 && (MSize)pc < retry_record_pt->sizebc);
    if (bc_op(proto_bc(retry_record_pt)[pc]) == BC_LEN)
      retry_record_len_calls++;
  }
  return 0;
}

/* Revalidate the actual immutable native trace through both independent
** certificates, then corrupt the helper, retry guard and temporary root in a
** private copy. The published trace is never modified. */
static void exercise_arm64_tablen_admission(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCtrace *T = NULL;
  GCproto *pt;
  IRIns *copy;
  LJArm64PostRAView v = {0};
  IRRef call = 0, guard = 0, root = 0, retry = 0, semantic_nins;
  IRRef table = 0, r;
  {
    TraceNo no;
    for (no = 1; no < 256 && !T; no++) {
      GCtrace *candidate = traceref_safe(J, no);
      IRRef r;
      IRRef tabref = 0;
      int rooted = 0;
      if (!candidate || !trace_runnable_acq(candidate, no) ||
          bc_op(candidate->startins) != BC_LOOP)
        continue;
      for (r = REF_FIRST; r < candidate->nins; r++)
        if (candidate->ir[r].o == IR_SLOAD &&
            irt_type(candidate->ir[r].t) == IRT_TAB)
          tabref = r;
      for (r = REF_FIRST; r < candidate->nins; r++)
        if (candidate->ir[r].o == IR_CALLS &&
            candidate->ir[r].op2 == IRCALL_lj_tab_len_rooted_try)
          rooted++;
      if (rooted >= 2 && tabref) {
        T = candidate;
        table = tabref;
      }
    }
  }
  assert(T && trace_runnable_acq(T, T->traceno));
  pt = trace_startpt_acq(T);
  copy = (IRIns *)calloc(T->nins, sizeof(*copy));
  assert(copy);
  memcpy(copy + T->nk, T->ir + T->nk,
         (T->nins - T->nk) * sizeof(*copy));
  v.ir = copy;
  v.owner = T;
  v.snap = T->snap;
  v.snapmap = T->snapmap;
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

  for (r = REF_FIRST; r < T->nins; r++) {
    IRIns ir = copy[r];
    if (ir.o == IR_SLOAD && irt_type(ir.t) == IRT_TAB) table = r;
    if (ir.o == IR_CALLS && ir.op2 == IRCALL_lj_tab_len_rooted_try) {
      call = r;
      root = ir.op1;
    }
  }
  assert(call && root && copy[root].o == IR_TMPREF);
  retry = copy[call].op1;
  for (r = REF_FIRST; r < T->nins; r++)
    if (copy[r].o == IR_NE &&
        (copy[r].op1 == call || copy[r].op2 == call)) {
      guard = r;
      break;
    }
  assert(guard);
  retry = copy[guard].op1 == call ? copy[guard].op2 : copy[guard].op1;
  assert(irref_isk(retry) && copy[retry].o == IR_KINT &&
         copy[retry].i == LJ_TAB_LEN_RETRY);
  assert(lj_asm_arm64_postra_admit(&v, &semantic_nins));
  assert(semantic_nins <= T->nins);

  /* Prove the semantic contract on a full copied trace and private IR. A
  ** separately rooted function has identical loop bytecode, so admission
  ** checks a real BC_LOOP/prototype relationship without editing the live
  ** published prototype. Rebase snapshot PCs and restore semantic LOOP,
  ** XPOLL and trailing-rename representation from the post-RA copy. */
  {
    jit_State *fake = (jit_State *)calloc(1, sizeof(*fake));
    GCtrace semantic = *T;
    IRIns *semantic_ir = (IRIns *)calloc(T->nins, sizeof(*semantic_ir));
    SnapEntry *semantic_map = (SnapEntry *)malloc(
      T->nsnapmap * sizeof(*semantic_map));
    const BCIns *origpc = trace_startpc_acq(T);
    GCproto *semantic_pt;
    BCIns *pc;
    LJArm64IRReject reject;
    assert(fake && semantic_ir && semantic_map && origpc);
    lua_getglobal(L, "rooted_len_trace_clone");
    assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top - 1)));
    semantic_pt = funcproto(funcV(L->top - 1));
    lua_pop(L, 1);
    pc = proto_bc(semantic_pt) + (origpc - proto_bc(pt));
    assert(pc >= proto_bc(semantic_pt) &&
           pc < proto_bc(semantic_pt) + semantic_pt->sizebc &&
           (BCIns)la_load32_acq((const uint32_t *)pc) == T->startins);
    memcpy(semantic_ir + T->nk, copy + T->nk,
           (T->nins - T->nk) * sizeof(*semantic_ir));
    memcpy(semantic_map, T->snapmap,
           T->nsnapmap * sizeof(*semantic_map));
    for (r = 0; r < T->nsnap; r++) {
      SnapShot *snap = &T->snap[r];
      uint64_t pcbase;
      memcpy(&pcbase, &semantic_map[snap->mapofs + snap->nent],
             sizeof(pcbase));
      pcbase = ((((uint64_t)(uintptr_t)(proto_bc(semantic_pt) +
                 (((uintptr_t)(pcbase >> 8) -
                   (uintptr_t)proto_bc(pt)) / sizeof(BCIns)))) << 8) |
                (uint8_t)pcbase);
      memcpy(&semantic_map[snap->mapofs + snap->nent], &pcbase,
             sizeof(pcbase));
    }
    semantic.snapmap = semantic_map;
    trace_startpt_rel(&semantic, semantic_pt);
    setmref(semantic.startpc, pc);
    semantic.nins = T->nins;
    while (semantic.nins > REF_FIRST &&
           semantic_ir[semantic.nins - 1].o == IR_RENAME)
      semantic.nins--;
    for (r = REF_FIRST; r < T->nins; r++) {
      if (semantic_ir[r].o == IR_LOOP) {
        semantic_ir[r].op1 = 0;
        semantic_ir[r].op2 = 0;
      } else if (semantic_ir[r].o == IR_XPOLL) {
        semantic_ir[r].op1 = 1;
        semantic_ir[r].op2 = 0;
      }
    }
    semantic.ir = semantic_ir;
    fake->L = L;
    fake->pt = semantic_pt;
    fake->startpc = pc;
    fake->baseslot = 1 + LJ_FR2;
    for (r = REF_FIRST; r < semantic.nins; r++)
      if (semantic_ir[r].o == IR_LOOP) fake->loopref = r;
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));

    semantic_ir[call].op2 = IRCALL_lj_tab_len;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[call] = copy[call];

    semantic_ir[retry].i = LJ_TAB_LEN_RETRY + 1;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[retry] = copy[retry];

    semantic_ir[root].op2 = IRTMPREF_IN1 | IRTMPREF_IN2;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[root] = copy[root];

    semantic_ir[table].op2 = IRSLOAD_READONLY;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[table] = copy[table];

    semantic_ir[call + 1u].o = IR_NOP;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[call + 1u] = copy[call + 1u];

    /* Comparisons with malformed references reject before any attempted
    ** instruction lookup: forward, self, and beyond the IR view. */
    assert(guard + 1u < semantic.nins);
    semantic_ir[guard].op2 = guard + 1u;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[guard] = copy[guard];
    semantic_ir[guard].op2 = guard;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[guard] = copy[guard];
    semantic_ir[guard].op2 = semantic.nins + 7u;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[guard] = copy[guard];
    assert(semantic.nk > 0);
    semantic_ir[guard].op2 = semantic.nk - 1u;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic_ir[guard] = copy[guard];
    free(semantic_ir);
    free(semantic_map);
    free(fake);
  }

  copy[call].op2 = IRCALL_lj_tab_len;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[call] = T->ir[call];

  copy[retry].i = LJ_TAB_LEN_RETRY + 1;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[retry] = T->ir[retry];

  copy[root].op2 = IRTMPREF_IN1 | IRTMPREF_IN2;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[root] = T->ir[root];
  /* The scalar allocator certificate models INT and NUM spills only; the
  ** newly admitted 64-bit TAB and PGC references must stay in registers. */
  copy[table].s = SPS_FIRST;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[table] = T->ir[table];
  copy[root].s = SPS_FIRST;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[root] = T->ir[root];
  assert(guard + 1u < T->nins);
  copy[guard].op2 = guard + 1u;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[guard] = T->ir[guard];
  copy[guard].op2 = guard;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[guard] = T->ir[guard];
  copy[guard].op2 = T->nins + 7u;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[guard] = T->ir[guard];
  assert(v.nk > 0);
  copy[guard].op2 = v.nk - 1u;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[guard] = T->ir[guard];
  copy[call + 1u].o = IR_NOP;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  free(copy);
}
#endif

static void exercise_jit_retry_side_exit(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t, *replacement;
  TValue *array, *replacement_array;
  CleanState outer_clean;
  CleanState clean;
  WaitState waits;
#if LJ_TARGET_ARM64
  jit_State *J = G2J(G(L));
  GCtrace *len_trace = NULL;
  ExitNo loop_exit = (ExitNo)~0u;
#endif

  push_dense_table(L, 6);
  t = tabV(L->top - 1);
  array = lj_tab_array_acq(t);
  lua_pushvalue(L, -1);
  lua_setglobal(L, "rooted_len_trace_table");

  /* Keep a second, already-published and equally populated separated array
  ** rooted while the test hook makes it the source table's valid successor. */
  push_dense_table(L, 6);
  replacement = tabV(L->top - 1);
  replacement_array = lj_tab_array_acq(replacement);
  assert(array != replacement_array);

  run_script(L,
    "local threading = require('threading')\n"
    "local jutil = require('jit.util')\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "assert(threading.gcworkers(1) == 0)\n"
    "local function traced_len(t, n)\n"
    "  local sum, i = 0, 1\n"
    "  while i <= n do sum = sum + #t; i = i + 1 end\n"
    "  return sum\n"
    "end\n"
    "rooted_len_trace_clone = assert(loadstring('return function(t,n) local sum,i=0,1 while i<=n do sum=sum+#t; i=i+1 end return sum end'))()\n"
    "for i = 1, 8 do assert(traced_len(assert(rooted_len_trace_table), 80) == 480) end\n"
    "assert(jutil.traceinfo(1))\n"
    "rooted_len_trace_call = traced_len\n");

#if LJ_TARGET_ARM64
  {
    TraceNo no;
    for (no = 1; no < 256 && !len_trace; no++) {
      GCtrace *candidate = traceref_safe(J, no);
      IRRef r, loop = 0;
      int rooted = 0;
      SnapNo s;
      if (!candidate || !trace_runnable_acq(candidate, no) ||
	  bc_op(candidate->startins) != BC_LOOP)
	continue;
      for (r = REF_FIRST; r < candidate->nins; r++) {
	if (candidate->ir[r].o == IR_CALLS &&
	    candidate->ir[r].op2 == IRCALL_lj_tab_len_rooted_try)
	  rooted = 1;
	if (candidate->ir[r].o == IR_LOOP) loop = r;
      }
      if (rooted && loop) {
	for (s = 0; s < candidate->nsnap; s++)
	  if (candidate->snap[s].ref == loop) {
	    len_trace = candidate;
	    loop_exit = s;
	    break;
	  }
      }
    }
    assert(len_trace && loop_exit != (ExitNo)~0u);
  }
  exercise_arm64_tablen_admission(L);
#endif

  /* Sample the successful helper at its generation LP. ARM64's rooted helper
  ** uses its scoped table-read lease; x64 has a separate forjit path. */
  lj_tab_test_reset_len_rooted_try_calls();
  arm_len_hook(LEN_HOOK_OBSERVE, L, t, NULL, NULL, 0);
  lua_getglobal(L, "rooted_len_trace_call");
  lua_getglobal(L, "rooted_len_trace_table");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 2, 1, 0) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "rooted length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
  assert(len_hook.hits == 1u);
#if LJ_TARGET_ARM64
  assert(lj_tab_test_len_rooted_try_calls() > 0u);
#else
  assert(lj_tab_test_len_rooted_try_calls() == 0u);
#endif

  /* Exercise the generated helper under an existing vector-read scope. Its
  ** nested enter/leave must preserve the caller's exact depth and epoch.
  ** With hotexit=1 this also starts recording at the retried LEN. The first
  ** resumed instruction must use the TG recorder dispatch: skipping LEN
  ** leaves the inherited TAB slot inconsistent with its numeric result. */
#if LJ_TARGET_ARM64
  lua_getglobal(L, "rooted_len_trace_call");
  retry_record_func = lua_topointer(L, -1);
  retry_record_pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  retry_record_len_calls = 0;
  lua_pushcfunction(L, record_retry_len);
  lua_setglobal(L, "rooted_len_retry_record");
  run_script(L, "jit.attach(rooted_len_retry_record, 'record')");
#endif
  outer_clean = clean_state(L);
  lj_tab_read_enter(L2TG(L));
  clean = clean_state(L);
  waits = wait_state();
  arm_len_hook(LEN_HOOK_GENERATION, L, t, NULL, replacement_array, 0);
  lua_getglobal(L, "rooted_len_trace_call");
  lua_getglobal(L, "rooted_len_trace_table");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 2, 1, 0) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "traced length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
  assert(len_hook.hits == 1u && lj_tab_array_acq(t) == replacement_array);
#if LJ_TARGET_ARM64
  /* Observe the resumed bytecode itself, independent of runtime assertions
  ** or child publication (heap-effect side traces remain unsupported). */
  assert(retry_record_len_calls > 0);
  run_script(L,
    "jit.attach(rooted_len_retry_record)\n"
    "rooted_len_retry_record = nil\n");
  retry_record_func = NULL;
  retry_record_pt = NULL;
#endif
  assert_wait_state(waits);
  assert_clean(L, clean);

  /* TMPREF is the generated ABI's semantic root. Replacing that exact word
  ** after the generation LP must force another side exit, even though the
  ** table's paired vectors themselves remain current. */
  arm_len_hook(LEN_HOOK_ROOT, L, t, &L2TG(L)->tmptv, NULL,
	       tv_rawload(L->top - 1));
  lua_getglobal(L, "rooted_len_trace_call");
  lua_getglobal(L, "rooted_len_trace_table");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 2, 1, 0) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "root-change length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
  assert(len_hook.hits == 1u);
  assert_wait_state(waits);
  assert_clean(L, clean);

  lj_tab_read_leave(L2TG(L));
  assert_clean(L, outer_clean);

  /* The first generated helper call returned RETRY after the hook changed the
  ** generation. Its guard side-exited, and the interpreter replayed against
  ** the new valid generation. Restore the source before either table dies. */
  lj_tab_array_rel(t, array);
#if LJ_TARGET_ARM64
  /* Interrupt after admission, before native entry. The error unwind must
  ** leave no read scope behind, and the same trace must resume natively. */
  {
    global_State *g = G(L);
    TGState *tg = L2TG(L);
    uint64_t epoch = gc2_hs_epoch_acq(g);
    int32_t vmstate = lj_tg_vmstate_load_acq(tg);
    RootedLenPublisher publisher = {g, tg, epoch, 0, 0, 0};
    pthread_t worker;
    int status;
    CleanState before = clean_state(L);
    WaitState before_waits = wait_state();
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
    assert(pthread_create(&worker, NULL, publish_rooted_len_stopreq,
			  &publisher) == 0);
    status = call_rooted_len(L);
    assert(pthread_join(worker, NULL) == 0);
    assert(status == LUA_ERRRUN && lua_isstring(L, -1));
    assert(strstr(lua_tostring(L, -1),
		  "thread interrupted: VM shutdown") != NULL);
    lua_pop(L, 1);
    assert(la_load32_acq(&publisher.saw_stage) == 1 &&
	   la_load32_acq(&publisher.saw_jit_base) == 1 &&
	   la_load32_acq(&publisher.published) == 1);
    assert(lj_trace_test_root_entry_publishes() == 1);
    assert(lj_trace_test_exit_calls() == 1 &&
	   lj_trace_test_last_exitno() == loop_exit);
    assert(gc2_hs_epoch_acq(g) == epoch + 1u &&
	   lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u);
    assert(!gc2_hs_pending_acq(g) && !lj_tg_reqmask_acq(tg) &&
	   !lj_tg_poll_acq(tg));
    assert(lj_tg_flags_acq(tg) & TGF_STOPREQ);
    assert(lj_tg_load_jit_base(tg) == NULL &&
	   lj_tg_in_native_acq(tg) == 0 &&
	   lj_tg_vmstate_load_acq(tg) == vmstate);
    assert_clean(L, before);
    assert_wait_state(before_waits);
    clear_stopreq(tg);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    lj_tab_test_reset_len_rooted_try_calls();
    assert_rooted_len_result(L);
    assert_rooted_len_native(L, "after STOPREQ");
    assert_clean(L, before);
    assert_wait_state(before_waits);

    /* Keep both Lua roots live through a full collection, then prove the
    ** rooted helper still executes in the resumed native trace. */
    run_script(L, "assert(rooted_len_trace_call and rooted_len_trace_table)");
    lua_gc(L, LUA_GCCOLLECT, 0);
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    lj_tab_test_reset_len_rooted_try_calls();
    assert_rooted_len_result(L);
    assert_rooted_len_native(L, "after full GC");
    assert_clean(L, before);
    assert_wait_state(before_waits);
  }
#endif
  run_script(L,
    "assert(require('threading').gcworkers(0) == 1)\n"
    "rooted_len_trace_call = nil\n"
    "rooted_len_trace_table = nil\n");
  lua_settop(L, top);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *wrong;
  assert(L != NULL);
  luaL_openlibs(L);
  wrong = lua_newthread(L);  /* Rooted, but not claimed by this actor. */
  assert(wrong != NULL);

  lj_tab_test_reset_wait_no_l_calls();
  lj_tab_test_reset_wait_l_calls();
  lj_tab_test_reset_store_wait_l_calls();
  exercise_forjit_direct_rejections(L, wrong);
  exercise_success(L);
  exercise_admission_retries(L);
  exercise_structural_retries(L);
  exercise_root_and_owner_validation(L, wrong);
  exercise_jit_retry_side_exit(L);
  assert(lj_tab_test_wait_no_l_calls() == 0u);
  assert(lj_tab_test_wait_l_calls() == 0u);
  assert(lj_tab_test_store_wait_l_calls() == 0u);

  lua_close(L);
  puts("t-tab-rooted-len-try OK");
  return 0;
}
