/*
** Focused bounded authoritative-root table length tests.
*/

#include <assert.h>
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
    assert(lj_tab_test_len_rooted_try_calls() == 0u);
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

static void exercise_jit_retry_side_exit(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t, *replacement;
  TValue *array, *replacement_array;
  CleanState outer_clean;
  CleanState clean;
  WaitState waits;

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
    "local t = assert(rooted_len_trace_table)\n"
    "local function traced_len(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + #t end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 8 do assert(traced_len(80) == 480) end\n"
    "assert(jutil.traceinfo(1))\n"
    "rooted_len_trace_call = traced_len\n");

  /* Sample a successful helper at its generation LP and prove it bypasses the
  ** general SMR/table-body-lease ABI. */
  lj_tab_test_reset_len_rooted_try_calls();
  arm_len_hook(LEN_HOOK_OBSERVE, L, t, NULL, NULL, 0);
  lua_getglobal(L, "rooted_len_trace_call");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 1, 1, 0) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lease-free length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
  assert(len_hook.hits == 1u);
  assert(lj_tab_test_len_rooted_try_calls() == 0u);

  /* Exercise the generated helper under an existing vector-read scope. Its
  ** nested enter/leave must preserve the caller's exact depth and epoch. */
  outer_clean = clean_state(L);
  lj_tab_read_enter(L2TG(L));
  clean = clean_state(L);
  waits = wait_state();
  arm_len_hook(LEN_HOOK_GENERATION, L, t, NULL, replacement_array, 0);
  lua_getglobal(L, "rooted_len_trace_call");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 1, 1, 0) != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "traced length call failed");
    assert(0);
  }
  assert(lua_tointeger(L, -1) == 480);
  lua_pop(L, 1);
  assert(len_hook.hits == 1u && lj_tab_array_acq(t) == replacement_array);
  assert_wait_state(waits);
  assert_clean(L, clean);

  /* TMPREF is the generated ABI's semantic root. Replacing that exact word
  ** after the generation LP must force another side exit, even though the
  ** table's paired vectors themselves remain current. */
  arm_len_hook(LEN_HOOK_ROOT, L, t, &L2TG(L)->tmptv, NULL,
	       tv_rawload(L->top - 1));
  lua_getglobal(L, "rooted_len_trace_call");
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 1, 1, 0) != 0) {
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
