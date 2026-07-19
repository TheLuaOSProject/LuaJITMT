/*
** Focused bounded rooted/clocked VM-event handler preparation regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_vmevent.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-vmevent-prepare-clocked requires LJ_GC2_TEST_HELPERS"
#endif
#ifndef LJ_TAB_TEST_HELPERS
#error "t-vmevent-prepare-clocked requires LJ_TAB_TEST_HELPERS"
#endif

#ifndef LUAJIT_DISABLE_VMEVENT
typedef struct WaitState {
  uint32_t no_l;
  uint32_t l;
  uint32_t store_l;
} WaitState;

static uint32_t handler_a_calls;
static uint32_t handler_b_calls;
static uint32_t raw_meta_calls;
static uint32_t close_prepare_calls;

static int handler_a(lua_State *L)
{
  UNUSED(L);
  handler_a_calls++;
  return 0;
}

static int handler_b(lua_State *L)
{
  UNUSED(L);
  handler_b_calls++;
  return 0;
}

static int raw_meta_trap(lua_State *L)
{
  UNUSED(L);
  raw_meta_calls++;
  return 0;
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
#endif

static void assert_snapshot_zero(
  const LJJitEventAttachmentSnapshot *snapshot)
{
  assert(snapshot->sequence == 0);
  assert(snapshot->next_generation == 0);
  assert(snapshot->generation == 0);
}

#ifndef LUAJIT_DISABLE_VMEVENT
static int prepare_checked(lua_State *L, VMEvent ev,
			   LJVMEVENTPrepareResult *result, int bounded)
{
  ptrdiff_t oldtop = savestack(L, L->top);
  WaitState waits = wait_state();
  int status = lj_vmevent_prepare_try(L, ev, result);
  if (bounded)
    assert_wait_state(waits);
  if (status == LJ_VMEVENT_PREPARE_READY) {
    assert(result->argbase != 0);
    assert(savestack(L, L->top) == result->argbase);
  } else {
    assert(savestack(L, L->top) == oldtop);
    assert(result->argbase == 0);
  }
  assert(gc2_smr_readers_acq(G(L)) == 0);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return status;
}

static TValue *prepared_handler(lua_State *L,
				 const LJVMEVENTPrepareResult *result)
{
  TValue *handler = restorestack(L, result->argbase) - 1 - LJ_FR2;
#if LJ_FR2
  assert(tvisnil(restorestack(L, result->argbase) - 1));
#endif
  assert(tvisfunc(handler) && iscfunc(funcV(handler)));
  return handler;
}

static void discard_ready(lua_State *L, ptrdiff_t oldtop)
{
  L->top = restorestack(L, oldtop);
}
#endif

static void raw_get_events(lua_State *L)
{
  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  lua_rawget(L, LUA_REGISTRYINDEX);
}

#ifndef LUAJIT_DISABLE_VMEVENT
static void raw_set_events_from_top(lua_State *L)
{
  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  lua_pushvalue(L, -2);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

static void raw_set_events_nil(lua_State *L)
{
  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  lua_pushnil(L);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

static void raw_install_handler(lua_State *L, VMEvent ev, lua_CFunction fn)
{
  raw_get_events(L);
  assert(lua_istable(L, -1));
  if (fn)
    lua_pushcfunction(L, fn);
  else
    lua_pushnil(L);
  lua_rawseti(L, -2, VMEVENT_HASH(ev));
  lua_pop(L, 1);
}

static void raw_install_nonfunction(lua_State *L, VMEvent ev)
{
  raw_get_events(L);
  assert(lua_istable(L, -1));
  lua_pushinteger(L, 17);
  lua_rawseti(L, -2, VMEVENT_HASH(ev));
  lua_pop(L, 1);
}

static void call_jit_attach(lua_State *L, lua_CFunction fn,
			    const char *event)
{
  int top = lua_gettop(L);
  lua_getglobal(L, "jit");
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "attach");
  lua_remove(L, -2);
  assert(lua_isfunction(L, -1));
  if (fn == handler_a)
    lua_getglobal(L, "lj_reader_handler_a");
  else if (fn == handler_b)
    lua_getglobal(L, "lj_reader_handler_b");
  else
    assert(0);
  assert(lua_iscfunction(L, -1) && lua_tocfunction(L, -1) == fn);
  if (event)
    lua_pushstring(L, event);
  ljt_lua_pcall(L, event ? 2 : 1, 0, "jit.attach");
  assert(lua_gettop(L) == top);
}

static uint32_t accepted_state_initial(void)
{
#if LJ_HASJIT
  return LJ_VMEVENT_ATTACHMENT_INITIAL;
#else
  return LJ_VMEVENT_ATTACHMENT_UNCLOCKED;
#endif
}

static uint32_t accepted_state_published(void)
{
#if LJ_HASJIT
  return LJ_VMEVENT_ATTACHMENT_PUBLISHED;
#else
  return LJ_VMEVENT_ATTACHMENT_UNCLOCKED;
#endif
}

static void assert_result_accepted(const LJVMEVENTPrepareResult *result,
				   uint32_t state, uint32_t slot)
{
  assert(result->attachment_state == state);
  assert(result->slot == slot);
  if (state == LJ_VMEVENT_ATTACHMENT_INITIAL ||
      state == LJ_VMEVENT_ATTACHMENT_UNCLOCKED)
    assert_snapshot_zero(&result->attachment);
  else {
    assert((result->attachment.sequence & 1u) == 0);
    assert(result->attachment.sequence != 0);
    assert(result->attachment.next_generation == result->attachment.generation);
    assert(result->attachment.generation != 0);
  }
}

static void assert_result_retry(const LJVMEVENTPrepareResult *result)
{
  assert(result->argbase == 0);
  assert(result->slot == LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE);
  assert(result->attachment_state == LJ_VMEVENT_ATTACHMENT_INVALID);
  assert_snapshot_zero(&result->attachment);
}

static void test_fixed_bootstrap_key(lua_State *L)
{
  global_State *g = G(L);
  GCstr *key = (GCstr *)la_loadptr_acq(
    (void *const *)&g->main_tg->vmevent_regkey);
  TGState *secondary = (TGState *)malloc(sizeof(*secondary));
  assert(key != NULL && key->gct == (uint8_t)~LJ_TSTR);
  assert(key->len == sizeof(LJ_VMEVENTS_REGKEY) - 1u);
  assert(memcmp(strdata(key), LJ_VMEVENTS_REGKEY,
		key->len) == 0);
  assert((key->marked & LJ_GC_FIXED) != 0);

  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  assert(tvisstr(L->top - 1) && strV(L->top - 1) == key);
  lua_pop(L, 1);

  assert(secondary != NULL);
  lj_tg_init_thread(g, secondary, NULL, 0);
  assert(la_loadptr_acq((void *const *)&secondary->vmevent_regkey) == NULL);
  assert(lj_tg_fini_thread(g, secondary));
  free(secondary);
}

static void test_initial_default_and_raw(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  ptrdiff_t oldtop;
  int status;

  /* luaL_newstate() installs ERRFIN directly, before any clock writer. */
  oldtop = savestack(L, L->top);
  status = prepare_checked(L, LJ_VMEVENT_ERRFIN, &result, 1);
  assert(status == LJ_VMEVENT_PREPARE_READY);
  assert_result_accepted(&result, accepted_state_initial(),
			 (uint32_t)LJ_VMEVENT_ERRFIN & 7u);
  assert(prepared_handler(L, &result) != NULL);
  discard_ready(L, oldtop);

  raw_install_handler(L, LJ_VMEVENT_TRACE, handler_a);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  oldtop = savestack(L, L->top);
  status = prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1);
  assert(status == LJ_VMEVENT_PREPARE_READY);
  assert_result_accepted(&result, accepted_state_initial(), 1);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  /* The ABI wrapper remains exactly the legacy argbase-or-zero surface. */
  oldtop = savestack(L, L->top);
  assert(lj_vmevent_prepare(L, LJ_VMEVENT_TRACE) != 0);
  assert(funcV(L->top - 1 - LJ_FR2)->c.f == handler_a);
  discard_ready(L, oldtop);

  raw_install_handler(L, LJ_VMEVENT_TRACE, NULL);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  status = prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1);
  assert(status == LJ_VMEVENT_PREPARE_ABSENT);
  assert_result_accepted(&result, accepted_state_initial(), 1);
  assert((vmevmask_load_acq(g) & VMEVENT_MASK(LJ_VMEVENT_TRACE)) == 0);

  raw_install_nonfunction(L, LJ_VMEVENT_TRACE);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  status = prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1);
  assert(status == LJ_VMEVENT_PREPARE_ABSENT);
  assert_result_accepted(&result, accepted_state_initial(), 1);

  memset(&result, 0xa5, sizeof(result));
  oldtop = savestack(L, L->top);
  assert(lj_vmevent_prepare_try(L, (VMEvent)0, &result) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(savestack(L, L->top) == oldtop);
  assert_result_retry(&result);
  assert(lj_vmevent_prepare_try(L, LJ_VMEVENT_TRACE, NULL) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(savestack(L, L->top) == oldtop);
}

static void test_raw_metamethod_bypass(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;

  raw_get_events(L);
  assert(lua_istable(L, -1));
  lua_newtable(L);
  lua_pushcfunction(L, raw_meta_trap);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, raw_meta_trap);
  lua_setfield(L, -2, "__newindex");
  assert(lua_setmetatable(L, -2));
  lua_pushnil(L);
  lua_rawseti(L, -2, VMEVENT_HASH(LJ_VMEVENT_TRACE));
  lua_pop(L, 1);

  raw_meta_calls = 0;
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_ABSENT);
  assert(raw_meta_calls == 0);

  raw_get_events(L);
  lua_pushnil(L);
  assert(lua_setmetatable(L, -2));
  lua_pop(L, 1);
}

static void test_smr_retry(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  ptrdiff_t top = savestack(L, L->top);
  uint8_t mask;
  uint32_t expect = LJ_GC2_SMR_OPEN;

  raw_install_handler(L, LJ_VMEVENT_TRACE, handler_a);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  mask = vmevmask_load_acq(g);
  assert(gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top);
  assert(vmevmask_load_acq(g) == mask);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
}

typedef enum PrepareHookAction {
  PREPARE_HOOK_GROW,
  PREPARE_HOOK_REENTER,
  PREPARE_HOOK_REPLACE_REGISTRY,
  PREPARE_HOOK_ATTACH_A,
  PREPARE_HOOK_ATTACH_B,
  PREPARE_HOOK_DETACH_A_GC,
  PREPARE_HOOK_DETACH_B_GC,
  PREPARE_HOOK_ASSERT_ADMISSION_CONSUMED,
  PREPARE_HOOK_ARM_EVENT_TABLE_ADMISSION,
#if LJ_HASJIT
  PREPARE_HOOK_ADVANCE_CLOCK_MASK,
#else
  PREPARE_HOOK_CLOSE_SMR_MASK,
#endif
} PrepareHookAction;

typedef struct PrepareHookCtx {
  int target;
  PrepareHookAction action;
  uint32_t visits;
  uint32_t target_hits;
  uint32_t nested_calls;
  uint32_t readers;
  uint32_t anchors;
  MSize old_stacksize;
  TValue *old_stack;
  int smr_closed;
} PrepareHookCtx;

static void prepare_stage_hook(lua_State *L, VMEvent ev, int stage, void *ud);

static void rearm_prepare_hook(PrepareHookCtx *ctx)
{
  lj_vmevent_test_set_prepare_hook(prepare_stage_hook, ctx);
}

static void prepare_hook_replace_registry(lua_State *L)
{
  lua_newtable(L);
  lua_pushcfunction(L, handler_b);
  lua_rawseti(L, -2, VMEVENT_HASH(LJ_VMEVENT_TRACE));
  raw_set_events_from_top(L);
  lua_pop(L, 1);
  /* The old table and handler now survive solely through the reader's two
  ** published stack roots. */
  lua_gc(L, LUA_GCCOLLECT, 0);
}

#if LJ_HASJIT
static void prepare_hook_advance_clock_mask(lua_State *L)
{
  global_State *g = G(L);
  LJJitEventAttachmentClock *clock =
    &g->main_tg->jit_event_attachment[(uint32_t)LJ_VMEVENT_TRACE & 7u];
  LJJitEventAttachmentSnapshot snapshot;
  assert(lj_jit_event_attachment_snapshot(
    g, (uint32_t)LJ_VMEVENT_TRACE & 7u, &snapshot) ==
    LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED);
  assert(snapshot.sequence <= UINT64_MAX - 2u);
  assert(snapshot.generation != UINT64_MAX);
  /* Simulate an independent peer changing an unrelated bit and completing a
  ** new canonical publication. The reader may restore TRACE only. */
  vmevmask_store_rel(g, VMEVENT_MASK(LJ_VMEVENT_BC));
  la_store64_rel(&clock->next_generation, snapshot.generation + 1u);
  la_store64_rel(&clock->generation, snapshot.generation + 1u);
  la_store64_rel(&clock->sequence, snapshot.sequence + 2u);
}
#else
static void prepare_hook_close_smr_mask(lua_State *L, PrepareHookCtx *ctx)
{
  global_State *g = G(L);
  uint32_t expect = LJ_GC2_SMR_OPEN;
  vmevmask_store_rel(g, VMEVENT_MASK(LJ_VMEVENT_BC));
  assert(gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  ctx->smr_closed = 1;
}
#endif

static void prepare_stage_hook(lua_State *L, VMEvent ev, int stage, void *ud)
{
  PrepareHookCtx *ctx = (PrepareHookCtx *)ud;
  global_State *g = G(L);
  GCstr *key = (GCstr *)la_loadptr_acq(
    (void *const *)&g->main_tg->vmevent_regkey);
  TValue *keyroot = L->top - 2;
  ctx->visits++;

  /* Every callout is outside the bounded getter's SMR/lease transaction, and
  ** all four temporary semantic roots remain enumerated. */
  assert(gc2_smr_readers_acq(g) == ctx->readers);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == ctx->anchors);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(tvisstr(keyroot) && strV(keyroot) == key);
  assert(L->top - (3 + LJ_FR2) < keyroot);

  if (stage != ctx->target) {
    /* The production hook was taken before this callback. Explicitly arm the
    ** same one-shot for the next stage. */
    rearm_prepare_hook(ctx);
    return;
  }
  ctx->target_hits++;
  assert(ctx->target_hits == 1u);

  switch (ctx->action) {
  case PREPARE_HOOK_GROW:
    ctx->old_stacksize = L->stacksize;
    ctx->old_stack = tvref(L->stack);
    assert(lua_checkstack(L, (int)(2u * L->stacksize)));
    assert(L->stacksize > ctx->old_stacksize);
    assert(tvref(L->stack) != ctx->old_stack);
    lua_pushliteral(L, "hook scratch after relocation");
    break;
  case PREPARE_HOOK_REENTER: {
    LJVMEVENTPrepareResult nested;
    ptrdiff_t top = savestack(L, L->top);
    int status = lj_vmevent_prepare_try(L, ev, &nested);
    assert(status == LJ_VMEVENT_PREPARE_READY);
    assert(funcV(prepared_handler(L, &nested))->c.f == handler_a);
    L->top = restorestack(L, top);
    ctx->nested_calls++;
    break;
  }
  case PREPARE_HOOK_REPLACE_REGISTRY:
    prepare_hook_replace_registry(L);
    break;
  case PREPARE_HOOK_ATTACH_A:
    call_jit_attach(L, handler_a, "trace");
    break;
  case PREPARE_HOOK_ATTACH_B:
    call_jit_attach(L, handler_b, "trace");
    break;
  case PREPARE_HOOK_DETACH_A_GC:
    call_jit_attach(L, handler_a, NULL);
    lua_gc(L, LUA_GCCOLLECT, 0);
    break;
  case PREPARE_HOOK_DETACH_B_GC:
    call_jit_attach(L, handler_b, NULL);
    lua_gc(L, LUA_GCCOLLECT, 0);
    break;
  case PREPARE_HOOK_ASSERT_ADMISSION_CONSUMED:
    assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
    break;
  case PREPARE_HOOK_ARM_EVENT_TABLE_ADMISSION:
    assert(tvistab(L->top - 1));
    lj_gc2_test_stack_admission_retry_once(obj2gco(tabV(L->top - 1)));
    break;
#if LJ_HASJIT
  case PREPARE_HOOK_ADVANCE_CLOCK_MASK:
    prepare_hook_advance_clock_mask(L);
    break;
#else
  case PREPARE_HOOK_CLOSE_SMR_MASK:
    prepare_hook_close_smr_mask(L, ctx);
    break;
#endif
  default:
    assert(0);
  }
}

static void arm_prepare_hook(lua_State *L, PrepareHookCtx *ctx, int stage,
			     PrepareHookAction action)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->target = stage;
  ctx->action = action;
  ctx->readers = gc2_smr_readers_acq(G(L));
  ctx->anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  rearm_prepare_hook(ctx);
}

static void assert_hook_done(const PrepareHookCtx *ctx)
{
  assert(ctx->visits != 0);
  assert(ctx->target_hits == 1u);
  lj_vmevent_test_set_prepare_hook(NULL, NULL);
}

static void test_stack_rehome_reentry_and_capture(lua_State *L)
{
  LJVMEVENTPrepareResult result;
  PrepareHookCtx hook;
  ptrdiff_t oldtop;

  raw_install_handler(L, LJ_VMEVENT_TRACE, handler_a);
  oldtop = savestack(L, L->top);
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_CLOCK_A,
		   PREPARE_HOOK_GROW);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert_hook_done(&hook);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  oldtop = savestack(L, L->top);
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP,
		   PREPARE_HOOK_REENTER);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert_hook_done(&hook);
  assert(hook.nested_calls == 1u);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  /* A raw registry replacement is intentionally unclocked/racy, but the
  ** current observation must finish from its one captured table safely. */
  oldtop = savestack(L, L->top);
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP,
		   PREPARE_HOOK_REPLACE_REGISTRY);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert_hook_done(&hook);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_b);
  discard_ready(L, oldtop);
}

static void test_registry_absence(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  int ref;
  raw_get_events(L);
  assert(lua_istable(L, -1));
  ref = luaL_ref(L, LUA_REGISTRYINDEX);
  raw_set_events_nil(L);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_ABSENT);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  assert(lua_istable(L, -1));
  raw_set_events_from_top(L);
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

static void test_published_and_runtime_joff(lua_State *L)
{
  LJVMEVENTPrepareResult result;
  ptrdiff_t oldtop;

  call_jit_attach(L, handler_a, "trace");
  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert_result_accepted(&result, accepted_state_published(), 1);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  ljt_lua_dostring(L, "jit.off()");
  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert_result_accepted(&result, accepted_state_published(), 1);
  discard_ready(L, oldtop);
}

static void test_all_published_lanes(lua_State *L)
{
  static const struct {
    VMEvent ev;
    const char *name;
    uint32_t slot;
  } cases[] = {
    { LJ_VMEVENT_BC, "bc", 0 },
    { LJ_VMEVENT_TRACE, "trace", 1 },
    { LJ_VMEVENT_RECORD, "record", 2 },
    { LJ_VMEVENT_TEXIT, "texit", 3 },
    { LJ_VMEVENT_ERRFIN, "errfin", 4 }
  };
  LJVMEVENTPrepareResult result;
  size_t i;
  for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    ptrdiff_t oldtop;
    call_jit_attach(L, handler_a, cases[i].name);
    /* Same-value publication is still a real clock generation. */
    if (cases[i].ev == LJ_VMEVENT_TRACE)
      call_jit_attach(L, handler_a, cases[i].name);
    oldtop = savestack(L, L->top);
    assert(prepare_checked(L, cases[i].ev, &result, 1) ==
	   LJ_VMEVENT_PREPARE_READY);
    assert_result_accepted(&result, accepted_state_published(),
			   cases[i].slot);
    assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
    discard_ready(L, oldtop);
  }
  call_jit_attach(L, handler_a, NULL);
}

static void test_per_hop_admission_retry(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  PrepareHookCtx hook;
  GCtab *registry_table;
  GCtab *events;
  GCstr *fixed_key;
  GCfunc *handler;
  ptrdiff_t top;
  uint8_t mask;

  call_jit_attach(L, handler_a, "trace");
  registry_table = lj_registry_tab_acq(g);
  fixed_key = (GCstr *)la_loadptr_acq(
    (void *const *)&g->main_tg->vmevent_regkey);
  assert(registry_table != NULL && fixed_key != NULL);
  raw_get_events(L);
  assert(tvistab(L->top - 1));
  events = tabV(L->top - 1);
  lua_pop(L, 1);
  lua_getglobal(L, "lj_reader_handler_a");
  assert(tvisfunc(L->top - 1));
  handler = funcV(L->top - 1);
  lua_pop(L, 1);

  top = savestack(L, L->top);
  mask = vmevmask_load_acq(g);
  lj_gc2_test_stack_admission_retry_once(obj2gco(registry_table));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);

  lj_gc2_test_stack_admission_retry_once(obj2gco(fixed_key));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);

  /* This exact event-table admission is consumed as hop 1's result lease.
  ** The AFTER_REGISTRY hook proves the hit occurred before hop 2 could start. */
  lj_gc2_test_stack_admission_retry_once(obj2gco(events));
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP,
		   PREPARE_HOOK_ASSERT_ADMISSION_CONSUMED);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_hook_done(&hook);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);

  /* Arm only after hop 1 has release-published the captured event table, so
  ** the exact failure is hop 2's parent-table lease, not hop 1's result. */
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_REGISTRY_LOOKUP,
		   PREPARE_HOOK_ARM_EVENT_TABLE_ADMISSION);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_hook_done(&hook);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);

  lj_gc2_test_stack_admission_retry_once(obj2gco(handler));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);
}

static void test_no_runtime_string_admission(lua_State *L)
{
  global_State *g = G(L);
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  LJVMEVENTPrepareResult result;
  ptrdiff_t oldtop;
  uint32_t expect = 0;
  assert(hdr != NULL);
  call_jit_attach(L, handler_a, "trace");
  assert(la_cas32(&hdr->resize, &expect, (uint32_t)0x80000000u,
		  LA_ACQ_REL, LA_ACQ));
  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);
  la_store32_rel(&hdr->resize, 0);
}

static void test_writer_reader_races(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  PrepareHookCtx hook;
  ptrdiff_t oldtop;

  call_jit_attach(L, handler_a, "trace");
  oldtop = savestack(L, L->top);
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_EVENT_LOOKUP,
		   PREPARE_HOOK_ATTACH_B);
#if LJ_HASJIT
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
#else
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);
#endif
  assert_hook_done(&hook);
  assert(savestack(L, L->top) == oldtop);

  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_b);
  discard_ready(L, oldtop);

  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_EVENT_LOOKUP,
		   PREPARE_HOOK_DETACH_B_GC);
#if LJ_HASJIT
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
#else
  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_b);
  discard_ready(L, oldtop);
#endif
  assert_hook_done(&hook);

  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_BEFORE_MASK_CLEAR,
		   PREPARE_HOOK_ATTACH_A);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert_hook_done(&hook);
  assert((vmevmask_load_acq(g) & VMEVENT_MASK(LJ_VMEVENT_TRACE)) != 0);

  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  discard_ready(L, oldtop);

  call_jit_attach(L, handler_a, NULL);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_MASK_CLEAR,
		   PREPARE_HOOK_ATTACH_B);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert_hook_done(&hook);
  assert((vmevmask_load_acq(g) & VMEVENT_MASK(LJ_VMEVENT_TRACE)) != 0);

  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_b);
  discard_ready(L, oldtop);

  call_jit_attach(L, handler_b, NULL);
  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_ABSENT);
  assert_result_accepted(&result, accepted_state_published(), 1);
}

static void test_bounded_bit_restore(lua_State *L)
{
  global_State *g = G(L);
  LJVMEVENTPrepareResult result;
  PrepareHookCtx hook;
  uint8_t expected = (uint8_t)(VMEVENT_MASK(LJ_VMEVENT_BC) |
			       VMEVENT_MASK(LJ_VMEVENT_TRACE));

  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
#if LJ_HASJIT
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_MASK_CLEAR,
		   PREPARE_HOOK_ADVANCE_CLOCK_MASK);
#else
  arm_prepare_hook(L, &hook, LJ_VMEVENT_TEST_AFTER_MASK_CLEAR,
		   PREPARE_HOOK_CLOSE_SMR_MASK);
#endif
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 0) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert_hook_done(&hook);
  assert(vmevmask_load_acq(g) == expected);
#if !LJ_HASJIT
  assert(hook.smr_closed);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
#endif
}

#if LJ_HASJIT
static void clock_store(LJJitEventAttachmentClock *clock, uint64_t sequence,
			uint64_t next_generation, uint64_t generation)
{
  la_store64_rel(&clock->next_generation, next_generation);
  la_store64_rel(&clock->generation, generation);
  la_store64_rel(&clock->sequence, sequence);
}

static void test_clock_retry_shapes(lua_State *L)
{
  global_State *g = G(L);
  LJJitEventAttachmentClock *clock =
    &g->main_tg->jit_event_attachment[(uint32_t)LJ_VMEVENT_TRACE & 7u];
  LJJitEventAttachmentSnapshot saved;
  LJVMEVENTPrepareResult result;
  ptrdiff_t top = savestack(L, L->top);
  uint8_t mask;
  assert(lj_jit_event_attachment_snapshot(g, 1, &saved) ==
	 LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED);

  (void)vmevmask_set_bits_acqrel(g, VMEVENT_MASK(LJ_VMEVENT_TRACE));
  mask = vmevmask_load_acq(g);
  clock_store(clock, saved.sequence + 1u, saved.generation,
	      saved.generation);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);

  clock_store(clock, saved.sequence, saved.generation + 1u,
	      saved.generation);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert_result_retry(&result);
  assert(savestack(L, L->top) == top && vmevmask_load_acq(g) == mask);
  clock_store(clock, saved.sequence, saved.next_generation,
	      saved.generation);
}
#endif

static void test_result_root_survives_detach_gc(lua_State *L)
{
  LJVMEVENTPrepareResult result;
  TValue *handler;
  GCfunc *accepted;
  ptrdiff_t oldtop;
  int weakref;
  int handler_index;
  uint32_t calls = handler_a_calls;

  /* Keep a weak oracle for the exact closure. It must stay populated only
  ** because the prepared handler slot is a real enumerated strong root. */
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  assert(lua_setmetatable(L, -2));
  lua_getglobal(L, "lj_reader_handler_a");
  assert(lua_iscfunction(L, -1) && lua_tocfunction(L, -1) == handler_a);
  lua_rawseti(L, -2, 1);
  weakref = luaL_ref(L, LUA_REGISTRYINDEX);

  call_jit_attach(L, handler_a, "trace");
  oldtop = savestack(L, L->top);
  assert(prepare_checked(L, LJ_VMEVENT_TRACE, &result, 1) ==
	 LJ_VMEVENT_PREPARE_READY);
  handler = prepared_handler(L, &result);
  assert(funcV(handler)->c.f == handler_a);
  accepted = funcV(handler);
  handler_index = (int)(handler - L->base) + 1;
  assert(handler_index > 0 && handler_index <= lua_gettop(L));
  call_jit_attach(L, handler_a, NULL);
  lua_pushnil(L);
  lua_setglobal(L, "lj_reader_handler_a");

  /* No event lane may retain this exact closure after exact detach. */
  raw_get_events(L);
  assert(lua_istable(L, -1));
  {
    static const VMEvent events[] = {
      LJ_VMEVENT_BC, LJ_VMEVENT_TRACE, LJ_VMEVENT_RECORD,
      LJ_VMEVENT_TEXIT, LJ_VMEVENT_ERRFIN
    };
    size_t i;
    for (i = 0; i < sizeof(events)/sizeof(events[0]); i++) {
      lua_rawgeti(L, -1, VMEVENT_HASH(events[i]));
      assert(!lua_isfunction(L, -1) ||
	     !tvisfunc(L->top - 1) || funcV(L->top - 1) != accepted);
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  handler = prepared_handler(L, &result);
  assert(funcV(handler) == accepted && accepted->c.f == handler_a);

  lua_rawgeti(L, LUA_REGISTRYINDEX, weakref);
  assert(lua_istable(L, -1));
  lua_rawgeti(L, -1, 1);
  assert(lua_iscfunction(L, -1));
  assert(tvisfunc(L->top - 1) && funcV(L->top - 1) == accepted);
  assert(lua_rawequal(L, handler_index, -1));
  lua_pop(L, 2);

  L->top = handler + 1;
  ljt_lua_pcall(L, 0, 0, "rooted detached VM-event handler");
  assert(handler_a_calls == calls + 1u);
  discard_ready(L, oldtop);
  luaL_unref(L, LUA_REGISTRYINDEX, weakref);
}

static int close_prepare_probe(lua_State *L)
{
  LJVMEVENTPrepareResult result;
  ptrdiff_t top = savestack(L, L->top);
  assert(lj_vmevent_prepare_try(L, LJ_VMEVENT_TRACE, &result) ==
	 LJ_VMEVENT_PREPARE_READY);
  assert(funcV(prepared_handler(L, &result))->c.f == handler_a);
  L->top = restorestack(L, top);
  close_prepare_calls++;
  return 0;
}

static void test_real_close_finalizer(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  close_prepare_calls = 0;
  lua_pushcfunction(L, handler_a);
  lua_setglobal(L, "lj_reader_close_handler");
  lua_pushcfunction(L, close_prepare_probe);
  lua_setglobal(L, "lj_reader_close_probe");
  ljt_lua_dostring(L,
    "jit.attach(lj_reader_close_handler, 'trace')\n"
    "local p = newproxy(true)\n"
    "getmetatable(p).__gc = lj_reader_close_probe\n"
    "_G.lj_reader_close_proxy = p\n");
  lua_close(L);
  assert(close_prepare_calls == 1u);
}

static void run_enabled_tests(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  test_fixed_bootstrap_key(L);
  test_initial_default_and_raw(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, handler_a);
  lua_setglobal(L, "lj_reader_handler_a");
  lua_pushcfunction(L, handler_b);
  lua_setglobal(L, "lj_reader_handler_b");
  test_raw_metamethod_bypass(L);
  test_smr_retry(L);
  test_stack_rehome_reentry_and_capture(L);
  test_registry_absence(L);
  test_published_and_runtime_joff(L);
  test_all_published_lanes(L);
  test_per_hop_admission_retry(L);
  test_no_runtime_string_admission(L);
  test_writer_reader_races(L);
  test_bounded_bit_restore(L);
#if LJ_HASJIT
  test_clock_retry_shapes(L);
#endif
  test_result_root_survives_detach_gc(L);
  lua_close(L);
  test_real_close_finalizer();
}
#endif /* !LUAJIT_DISABLE_VMEVENT */

#ifdef LUAJIT_DISABLE_VMEVENT
static uint32_t disabled_hook_calls;

static void disabled_hook(lua_State *L, VMEvent ev, int stage, void *ud)
{
  UNUSED(L); UNUSED(ev); UNUSED(stage); UNUSED(ud);
  disabled_hook_calls++;
}

static void run_disabled_tests(void)
{
  lua_State *L = luaL_newstate();
  LJVMEVENTPrepareResult result;
  ptrdiff_t top;
  assert(L != NULL && G(L)->main_tg != NULL);
  assert(la_loadptr_acq(
    (void *const *)&G(L)->main_tg->vmevent_regkey) == NULL);
  raw_get_events(L);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  memset(&result, 0xa5, sizeof(result));
  top = savestack(L, L->top);
  disabled_hook_calls = 0;
  lj_vmevent_test_set_prepare_hook(disabled_hook, NULL);
  assert(lj_vmevent_prepare_try(L, LJ_VMEVENT_TRACE, &result) ==
	 LJ_VMEVENT_PREPARE_ABSENT);
  lj_vmevent_test_set_prepare_hook(NULL, NULL);
  assert(disabled_hook_calls == 0);
  assert(savestack(L, L->top) == top);
  assert(result.argbase == 0 && result.slot == 1u);
  assert(result.attachment_state == LJ_VMEVENT_ATTACHMENT_UNCLOCKED);
  assert_snapshot_zero(&result.attachment);
  assert(lj_vmevent_prepare(L, LJ_VMEVENT_TRACE) == 0);
  assert(savestack(L, L->top) == top);

  memset(&result, 0xa5, sizeof(result));
  assert(lj_vmevent_prepare_try(L, (VMEvent)0, &result) ==
	 LJ_VMEVENT_PREPARE_RETRY);
  assert(result.slot == LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE);
  assert(result.attachment_state == LJ_VMEVENT_ATTACHMENT_INVALID);
  assert_snapshot_zero(&result.attachment);
  lua_close(L);
}
#endif

int main(void)
{
#ifdef LUAJIT_DISABLE_VMEVENT
  run_disabled_tests();
#else
  run_enabled_tests();
#endif
  puts("t-vmevent-prepare-clocked OK");
  return 0;
}
