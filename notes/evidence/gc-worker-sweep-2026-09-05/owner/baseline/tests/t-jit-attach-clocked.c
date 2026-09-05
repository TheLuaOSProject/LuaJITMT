/*
** Focused production jit.attach() publication and compatibility regression.
*/

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tabtxn.h"
#include "lj_tg.h"
#include "lj_vmevent.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-jit-attach-clocked requires LJ_TAB_TEST_HELPERS"
#endif

static int attach_fn_a(lua_State *L)
{
  (void)L;
  return 0;
}

static void push_jit_attach(lua_State *L)
{
  lua_getglobal(L, "jit");
  assert(lua_istable(L, -1));
  lua_getfield(L, -1, "attach");
  assert(lua_isfunction(L, -1));
  lua_remove(L, -2);
}

#ifndef LUAJIT_DISABLE_VMEVENT

static int attach_fn_b(lua_State *L)
{
  (void)L;
  return 0;
}

static uint32_t event_metatable_hits;

static int event_metamethod_forbidden(lua_State *L)
{
  event_metatable_hits++;
  return luaL_error(L, "jit.attach touched event-table metamethod");
}

#endif

static int32_t event_key_len(const char *name, size_t len)
{
  const uint8_t *p = (const uint8_t *)name;
  uint32_t h = (uint32_t)len;
  while (*p)
    h = h ^ (lj_rol(h, 6) + *p++);
  return VMEVENT_HASHIDX(h);
}

static int32_t event_key(const char *name)
{
  return event_key_len(name, strlen(name));
}

static void test_event_hashes(void)
{
  static const char embedded_nul[] = "trace\0x";
  assert((uint32_t)event_key("bc") == 0x0001c418u);
  assert((uint32_t)event_key("trace") == 0x96c8a338u);
  assert((uint32_t)event_key("record") == 0x9425fa78u);
  assert((uint32_t)event_key("texit") == 0x94ef9580u);
  assert((uint32_t)event_key("errfin") == 0x96c9c440u);
  assert(event_key("bc") == VMEVENT_HASH(LJ_VMEVENT_BC));
  assert(event_key("trace") == VMEVENT_HASH(LJ_VMEVENT_TRACE));
  assert(event_key("record") == VMEVENT_HASH(LJ_VMEVENT_RECORD));
  assert(event_key("texit") == VMEVENT_HASH(LJ_VMEVENT_TEXIT));
  assert(event_key("errfin") == VMEVENT_HASH(LJ_VMEVENT_ERRFIN));
  assert((uint32_t)event_key_len(embedded_nul, 7) == 0xa6cab720u);
  assert((uint32_t)event_key("2694847501") == 0x94ef9580u);
  assert(event_key("2694847501") == event_key("texit"));
}

#ifndef LUAJIT_DISABLE_VMEVENT

static lua_Number number_from_bits(uint64_t bits)
{
  union {
    uint64_t bits;
    lua_Number number;
  } value;
  value.bits = bits;
  return value.number;
}

static int new_function_ref(lua_State *L, lua_CFunction fn)
{
  lua_pushcfunction(L, fn);
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void push_ref(lua_State *L, int ref)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
}

static GCfunc *function_ref_ptr(lua_State *L, int ref)
{
  GCfunc *fn;
  push_ref(L, ref);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  lua_pop(L, 1);
  return fn;
}

static void push_event_table(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
  assert(lua_istable(L, -1));
}

static int install_event_table(lua_State *L, int with_metatable)
{
  int ref;
  lua_newtable(L);
  if (with_metatable) {
    lua_newtable(L);
    lua_pushcfunction(L, event_metamethod_forbidden);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, event_metamethod_forbidden);
    lua_setfield(L, -2, "__index");
    assert(lua_setmetatable(L, -2));
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
  ref = luaL_ref(L, LUA_REGISTRYINDEX);
  return ref;
}

static GCtab *event_table_ptr(lua_State *L, int tabref)
{
  GCtab *tab;
  push_ref(L, tabref);
  assert(lua_istable(L, -1));
  tab = tabV(L->top-1);
  lua_pop(L, 1);
  return tab;
}

static void expect_event_table_ref(lua_State *L, int tabref)
{
  push_event_table(L);
  push_ref(L, tabref);
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
}

static void raw_clear_event_registry(lua_State *L)
{
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

static void expect_event_registry_nil(lua_State *L)
{
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  lua_pushliteral(L, LJ_VMEVENTS_REGKEY);
  lua_rawget(L, -2);
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
}

typedef struct CleanState {
  uint32_t readers;
  uint32_t anchors;
} CleanState;

static CleanState clean_state(lua_State *L)
{
  CleanState state;
  state.readers = gc2_smr_readers_acq(G(L));
  state.anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return state;
}

static void expect_clean_state(lua_State *L, CleanState state)
{
  assert(gc2_smr_readers_acq(G(L)) == state.readers);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == state.anchors);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

#if LJ_HASJIT
typedef struct ClockState {
  uint64_t sequence;
  uint64_t generation;
} ClockState;

static ClockState clock_state(global_State *g, uint32_t slot)
{
  LJJitEventAttachmentSnapshot snapshot;
  ClockState state;
  int rc = lj_jit_event_attachment_snapshot(g, slot, &snapshot);
  assert(rc == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL ||
	 rc == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_PUBLISHED);
  assert((snapshot.sequence & 1u) == 0);
  assert(snapshot.next_generation == snapshot.generation);
  if (rc == LJ_JIT_EVENT_ATTACHMENT_SNAPSHOT_INITIAL) {
    assert(snapshot.sequence == 0 && snapshot.generation == 0);
  } else {
    assert(snapshot.sequence != 0 && snapshot.generation != 0);
  }
  state.sequence = snapshot.sequence;
  state.generation = snapshot.generation;
  return state;
}

static void snapshot_clocks(global_State *g, ClockState *state)
{
  uint32_t slot;
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++)
    state[slot] = clock_state(g, slot);
}

static void expect_clock_delta(global_State *g, uint32_t slot,
			       ClockState before, uint64_t publications)
{
  ClockState after = clock_state(g, slot);
  assert(after.sequence == before.sequence + 2u * publications);
  assert(after.generation == before.generation + publications);
}

static void expect_clocks_same(global_State *g, const ClockState *before)
{
  uint32_t slot;
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++)
    expect_clock_delta(g, slot, before[slot], 0);
}
#endif

typedef enum AttachHookMode {
  ATTACH_HOOK_NONE,
  ATTACH_HOOK_SINGLE,
  ATTACH_HOOK_DETACH_SET
} AttachHookMode;

typedef struct AttachHookState {
  lua_State *L;
  global_State *g;
  GCtab *table;
  GCfunc *fn;
  CleanState clean;
  AttachHookMode mode;
  int32_t key;
  uint32_t slot;
  uint32_t hits;
  uint32_t detach_seen;
  uint8_t mask_before;
#if LJ_HASJIT
  ClockState clocks[LJ_JIT_EVENT_ATTACHMENT_SLOTS];
#endif
} AttachHookState;

static AttachHookState attach_hook;
static int light_key_tag;
static GCtab *detach_table_key;
static GCfunc *detach_function_key;
static GCudata *detach_userdata_key;
static GCtab *diverted_event_table;
static int diverted_event_ref = LUA_NOREF;
static uint32_t registry_diversion_hits;
static int key_int32(cTValue *key, int32_t *out);
#if LJ_HASJIT
static void expect_one_odd_clock(global_State *g, uint32_t odd_slot);
#endif

static int registry_divert_newindex(lua_State *L)
{
  size_t len;
  const char *key;
  assert(lua_gettop(L) == 3);
  assert(lua_istable(L, 1) && lua_istable(L, 3));
  key = lua_tolstring(L, 2, &len);
  assert(key != NULL && len == strlen(LJ_VMEVENTS_REGKEY));
  assert(memcmp(key, LJ_VMEVENTS_REGKEY, len) == 0);
  assert(diverted_event_ref == LUA_NOREF && diverted_event_table == NULL);
  diverted_event_table = tabV(L->base + 2);
  lua_pushvalue(L, 3);
  diverted_event_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  attach_hook.table = diverted_event_table;
  registry_diversion_hits++;
  return 0;
}

typedef struct AttachStaleHookState {
  lua_State *L;
  global_State *g;
  GCtab *table;
  GCfunc *oldfn;
  GCfunc *newfn;
  TValue *oldarray;
  TValue *newarray;
  MSize newasize;
  CleanState clean;
  uint32_t hits;
  uint8_t mask_before;
#if LJ_HASJIT
  ClockState clock_before;
#endif
} AttachStaleHookState;

static AttachStaleHookState stale_hook;

static void attach_committed_stale_hook(LJTabKeyedStoreTxn *txn)
{
  int32_t key;
  assert(txn != NULL && txn->owner_L == stale_hook.L);
  assert(txn->parent == stale_hook.table && txn->guard.g == stale_hook.g);
  assert(key_int32(&txn->key, &key));
  assert(key == VMEVENT_HASH(LJ_VMEVENT_BC));
  assert(tvisfunc(&txn->desired) && funcV(&txn->desired) == stale_hook.newfn);
  assert(gc2_smr_readers_acq(stale_hook.g) == stale_hook.clean.readers + 1u);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(stale_hook.L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  if (stale_hook.hits == 0) {
    assert(tvisfunc(&txn->expected) &&
	   funcV(&txn->expected) == stale_hook.oldfn);
    assert(vmevmask_load_acq(stale_hook.g) == stale_hook.mask_before);
  } else {
    assert(stale_hook.hits == 1);
    assert(tvisnil(&txn->expected));
    assert(vmevmask_load_acq(stale_hook.g) == VMEVENT_NOCACHE);
  }
#if LJ_HASJIT
  {
    LJJitEventAttachmentClock *clock =
      &stale_hook.g->main_tg->jit_event_attachment[0];
    assert(la_load64_acq(&clock->sequence) ==
	   stale_hook.clock_before.sequence + 2u * stale_hook.hits + 1u);
    assert(la_load64_acq(&clock->generation) ==
	   stale_hook.clock_before.generation + stale_hook.hits);
    assert(la_load64_acq(&clock->next_generation) ==
	   stale_hook.clock_before.generation + stale_hook.hits + 1u);
    expect_one_odd_clock(stale_hook.g, 0);
  }
#endif
  if (stale_hook.hits == 0) {
    assert(lj_tab_array_acq(stale_hook.table) == stale_hook.oldarray);
    lj_tab_asize_rel(stale_hook.table, stale_hook.newasize);
    lj_tab_array_rel(stale_hook.table, stale_hook.newarray);
  } else {
    assert(lj_tab_array_acq(stale_hook.table) == stale_hook.newarray);
  }
  stale_hook.hits++;
}

enum {
  DETACH_SEEN_BC = 1u << 0,
  DETACH_SEEN_TRACE = 1u << 1,
  DETACH_SEEN_RECORD = 1u << 2,
  DETACH_SEEN_ARRAY = 1u << 3,
  DETACH_SEEN_NEGDOUBLE = 1u << 4,
  DETACH_SEEN_NEGINT = 1u << 5,
  DETACH_SEEN_STRING = 1u << 6,
  DETACH_SEEN_BOOL = 1u << 7,
  DETACH_SEEN_TABLE = 1u << 8,
  DETACH_SEEN_LIGHTUD = 1u << 9,
  DETACH_SEEN_FUNCTION = 1u << 10,
  DETACH_SEEN_UDATA = 1u << 11
};

#define DETACH_SEEN_ALL ((1u << 12) - 1u)

static int key_int32(cTValue *key, int32_t *out)
{
  int64_t i64;
  if (tvisint(key)) {
    *out = intV(key);
    return 1;
  }
  if (tvisnum(key)) {
    int32_t i;
    if (lj_num2int_check(numV(key), i64, i)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static uint32_t detach_key_bit(global_State *g, cTValue *key)
{
  int32_t i;
  if (key_int32(key, &i)) {
    if (i == VMEVENT_HASH(LJ_VMEVENT_BC))
      return DETACH_SEEN_BC;
    if (i == VMEVENT_HASH(LJ_VMEVENT_TRACE))
      return DETACH_SEEN_TRACE;
    if (i == VMEVENT_HASH(LJ_VMEVENT_RECORD))
      return DETACH_SEEN_RECORD;
    if (i == 7)
      return DETACH_SEEN_ARRAY;
    if (i == -19)
      return DETACH_SEEN_NEGINT;
  }
  if (tvisnum(key) && numV(key) == -17.5)
    return DETACH_SEEN_NEGDOUBLE;
  if (tvisstr(key) && strcmp(strdata(strV(key)), "shape-string") == 0)
    return DETACH_SEEN_STRING;
  if (tvistrue(key))
    return DETACH_SEEN_BOOL;
  if (tvistab(key) && tabV(key) == detach_table_key)
    return DETACH_SEEN_TABLE;
  if (tvislightud(key) && lightudV(g, key) == &light_key_tag)
    return DETACH_SEEN_LIGHTUD;
  if (tvisfunc(key) && funcV(key) == detach_function_key)
    return DETACH_SEEN_FUNCTION;
  if (tvisudata(key) && udataV(key) == detach_userdata_key)
    return DETACH_SEEN_UDATA;
  return 0;
}

#if LJ_HASJIT
static void expect_one_odd_clock(global_State *g, uint32_t odd_slot)
{
  uint32_t slot;
  for (slot = 0; slot < LJ_JIT_EVENT_ATTACHMENT_SLOTS; slot++) {
    LJJitEventAttachmentClock *clock =
      &g->main_tg->jit_event_attachment[slot];
    uint64_t sequence = la_load64_acq(&clock->sequence);
    uint64_t next_generation = la_load64_acq(&clock->next_generation);
    uint64_t generation = la_load64_acq(&clock->generation);
    if (slot == odd_slot) {
      assert((sequence & 1u) != 0);
      assert(next_generation == generation + 1u);
    } else {
      assert((sequence & 1u) == 0);
      assert(next_generation == generation);
    }
  }
}
#endif

static void attach_post_cas_hook(LJTabKeyedStoreTxn *txn)
{
  int32_t key = 0;
  uint32_t slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  int clocked;
  assert(attach_hook.mode != ATTACH_HOOK_NONE);
  assert(txn != NULL && txn->owner_L == attach_hook.L);
  assert(txn->parent == attach_hook.table);
  assert(txn->guard.g == attach_hook.g);
  assert(gc2_smr_readers_acq(attach_hook.g) == attach_hook.clean.readers + 1u);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(attach_hook.L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  clocked = key_int32(&txn->key, &key) &&
    lj_jit_event_attachment_clock_slot(key, &slot);

  if (attach_hook.mode == ATTACH_HOOK_SINGLE) {
    assert(tvisfunc(&txn->desired));
    assert(funcV(&txn->desired) == attach_hook.fn);
    assert(key_int32(&txn->key, &key) && key == attach_hook.key);
    assert((clocked ? slot : LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE) ==
	   attach_hook.slot);
    assert(vmevmask_load_acq(attach_hook.g) == attach_hook.mask_before);
#if LJ_HASJIT
    if (clocked) {
      LJJitEventAttachmentClock *clock =
	&attach_hook.g->main_tg->jit_event_attachment[slot];
      assert(la_load64_acq(&clock->sequence) ==
	     attach_hook.clocks[slot].sequence + 1u);
      assert(la_load64_acq(&clock->next_generation) ==
	     attach_hook.clocks[slot].generation + 1u);
      assert(la_load64_acq(&clock->generation) ==
	     attach_hook.clocks[slot].generation);
      expect_one_odd_clock(attach_hook.g, slot);
    } else {
      expect_clocks_same(attach_hook.g, attach_hook.clocks);
    }
#else
    assert(!clocked || slot <= 4u);
#endif
  } else {
    uint32_t bit;
    assert(attach_hook.mode == ATTACH_HOOK_DETACH_SET);
    assert(tvisnil(&txn->desired));
    assert(tvisfunc(&txn->expected));
    assert(funcV(&txn->expected) == attach_hook.fn);
    bit = detach_key_bit(attach_hook.g, &txn->key);
    assert(bit != 0 && !(attach_hook.detach_seen & bit));
    attach_hook.detach_seen |= bit;
#if LJ_HASJIT
    if (clocked)
      expect_one_odd_clock(attach_hook.g, slot);
    else {
      uint32_t i;
      for (i = 0; i < LJ_JIT_EVENT_ATTACHMENT_SLOTS; i++)
	assert((la_load64_acq(
	  &attach_hook.g->main_tg->jit_event_attachment[i].sequence) & 1u) == 0);
    }
#else
    assert(!clocked || slot <= 4u);
#endif
  }
  attach_hook.hits++;
}

static void hook_begin_single(lua_State *L, int tabref, int fnref,
			      int32_t key, uint8_t mask)
{
  uint32_t slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  memset(&attach_hook, 0, sizeof(attach_hook));
  attach_hook.L = L;
  attach_hook.g = G(L);
  attach_hook.table = event_table_ptr(L, tabref);
  attach_hook.fn = function_ref_ptr(L, fnref);
  attach_hook.clean = clean_state(L);
  attach_hook.mode = ATTACH_HOOK_SINGLE;
  attach_hook.key = key;
  attach_hook.mask_before = mask;
  (void)lj_jit_event_attachment_clock_slot(key, &slot);
  attach_hook.slot = slot;
#if LJ_HASJIT
  snapshot_clocks(G(L), attach_hook.clocks);
#endif
  vmevmask_store_rel(G(L), mask);
  lj_tab_keyed_store_test_set_post_cas_hook(attach_post_cas_hook);
}

static void hook_end_single(lua_State *L)
{
  CleanState state = attach_hook.clean;
  assert(attach_hook.hits == 1);
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  attach_hook.mode = ATTACH_HOOK_NONE;
  expect_clean_state(L, state);
}

static int attach_string_status(lua_State *L, int fnref,
				const char *name, size_t len, int extra)
{
  int top = lua_gettop(L);
  int status;
  push_jit_attach(L);
  push_ref(L, fnref);
  lua_pushlstring(L, name, len);
  if (extra)
    lua_pushliteral(L, "ignored-extra-argument");
  status = lua_pcall(L, extra ? 3 : 2, 0, 0);
  if (status == LUA_OK)
    assert(lua_gettop(L) == top);
  else
    assert(lua_gettop(L) == top + 1);
  return status;
}

static void attach_string_ok(lua_State *L, int tabref, int fnref,
			     const char *name, size_t len, int extra,
			     uint8_t mask)
{
  hook_begin_single(L, tabref, fnref, event_key_len(name, len), mask);
  assert(attach_string_status(L, fnref, name, len, extra) == LUA_OK);
  hook_end_single(L);
  assert(vmevmask_load_acq(G(L)) == VMEVENT_NOCACHE);
}

static void attach_number_ok(lua_State *L, int tabref, int fnref,
			     lua_Number number, const char *formatted,
			     uint8_t mask)
{
  int top = lua_gettop(L);
  hook_begin_single(L, tabref, fnref, event_key(formatted), mask);
  push_jit_attach(L);
  push_ref(L, fnref);
  lua_pushnumber(L, number);
  assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
  assert(lua_gettop(L) == top);
  hook_end_single(L);
  assert(vmevmask_load_acq(G(L)) == VMEVENT_NOCACHE);
}

static void detach_ok(lua_State *L, int fnref)
{
  int top = lua_gettop(L);
  push_jit_attach(L);
  push_ref(L, fnref);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
  assert(lua_gettop(L) == top);
}

static void expect_raw_ref_int(lua_State *L, int32_t key, int ref)
{
  push_event_table(L);
  lua_pushinteger(L, key);
  lua_rawget(L, -2);
  push_ref(L, ref);
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 3);
}

static void expect_raw_nil_int(lua_State *L, int32_t key)
{
  push_event_table(L);
  lua_pushinteger(L, key);
  lua_rawget(L, -2);
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
}

static void raw_set_ref(lua_State *L, int tabref, int fnref,
			void (*push_key)(lua_State *L, void *ud), void *ud)
{
  push_ref(L, tabref);
  push_key(L, ud);
  push_ref(L, fnref);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

static void push_key_integer(lua_State *L, void *ud)
{
  lua_pushinteger(L, (lua_Integer)(intptr_t)ud);
}

static void push_key_number(lua_State *L, void *ud)
{
  lua_pushnumber(L, *(const lua_Number *)ud);
}

static void push_key_string(lua_State *L, void *ud)
{
  lua_pushstring(L, (const char *)ud);
}

static void push_key_bool(lua_State *L, void *ud)
{
  lua_pushboolean(L, (int)(intptr_t)ud);
}

static void push_key_ref(lua_State *L, void *ud)
{
  push_ref(L, (int)(intptr_t)ud);
}

static void push_key_lightud(lua_State *L, void *ud)
{
  lua_pushlightuserdata(L, ud);
}

static void expect_raw_nil_key(lua_State *L, int tabref,
			       void (*push_key)(lua_State *, void *), void *ud)
{
  push_ref(L, tabref);
  push_key(L, ud);
  lua_rawget(L, -2);
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
}

static void expect_raw_ref_key(lua_State *L, int tabref,
			       void (*push_key)(lua_State *, void *), void *ud,
			       int ref)
{
  push_ref(L, tabref);
  push_key(L, ud);
  lua_rawget(L, -2);
  push_ref(L, ref);
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 3);
}

static void seed_nonfunction(lua_State *L, int tabref,
			     const char *key, lua_Integer value)
{
  push_ref(L, tabref);
  lua_pushstring(L, key);
  lua_pushinteger(L, value);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

static void expect_nonfunction(lua_State *L, int tabref,
			       const char *key, lua_Integer value)
{
  push_ref(L, tabref);
  lua_pushstring(L, key);
  lua_rawget(L, -2);
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == value);
  lua_pop(L, 2);
}

static void test_public_attach_and_hash_compatibility(void)
{
  static const struct {
    const char *name;
    int32_t key;
    uint32_t slot;
  } events[] = {
    { "bc", (int32_t)0x0001c418u, 0 },
    { "trace", (int32_t)0x96c8a338u, 1 },
    { "record", (int32_t)0x9425fa78u, 2 },
    { "texit", (int32_t)0x94ef9580u, 3 },
    { "errfin", (int32_t)0x96c9c440u, 4 }
  };
  static const struct {
    uint64_t bits;
    const char *formatted;
  } numbers[] = {
    { UINT64_C(0x3ff8000000000000), "1.5" },
    { UINT64_C(0x8000000000000000), "-0" },
    { UINT64_C(0x7ff8000000000000), "nan" },
    { UINT64_C(0x7ff0000000000000), "inf" },
    { UINT64_C(0xfff0000000000000), "-inf" },
    { UINT64_C(0x4045000000000000), "42" }
  };
  static const char embedded_nul[] = "trace\0x";
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  int fn_a = new_function_ref(L, attach_fn_a);
  int fn_b = new_function_ref(L, attach_fn_b);
  int tabref;
  size_t i;
#if LJ_HASJIT
  ClockState before[LJ_JIT_EVENT_ATTACHMENT_SLOTS];
#endif

  event_metatable_hits = 0;
  tabref = install_event_table(L, 1);
  expect_event_table_ref(L, tabref);

  for (i = 0; i < sizeof(events)/sizeof(events[0]); i++) {
#if LJ_HASJIT
    ClockState clock0 = clock_state(g, events[i].slot);
#endif
    attach_string_ok(L, tabref, fn_a, events[i].name,
		     strlen(events[i].name), 0, (uint8_t)(0x20u + i));
    expect_event_table_ref(L, tabref);
    expect_raw_ref_int(L, events[i].key, fn_a);
#if LJ_HASJIT
    expect_clock_delta(g, events[i].slot, clock0, 1);
#endif
    assert(event_metatable_hits == 0);
  }

  /* A same-value attach is still an observable cache/clock publication. */
#if LJ_HASJIT
  {
    ClockState clock0 = clock_state(g, 1);
#endif
    attach_string_ok(L, tabref, fn_a, "trace", 5, 0, 0x31u);
#if LJ_HASJIT
    expect_clock_delta(g, 1, clock0, 1);
  }
#endif

  /* This decimal string is a stock hash collision with the TEXIT key. */
#if LJ_HASJIT
  {
    ClockState clock0 = clock_state(g, 3);
#endif
    attach_string_ok(L, tabref, fn_b, "2694847501", 10, 0, 0x32u);
    expect_raw_ref_int(L, VMEVENT_HASH(LJ_VMEVENT_TEXIT), fn_b);
#if LJ_HASJIT
    expect_clock_delta(g, 3, clock0, 1);
  }
#endif

  /* Full length seeds the hash, while byte mixing stops at the embedded NUL. */
#if LJ_HASJIT
  snapshot_clocks(g, before);
#endif
  attach_string_ok(L, tabref, fn_a, embedded_nul, 7, 0, 0x33u);
  expect_raw_ref_int(L, event_key_len(embedded_nul, 7), fn_a);
#if LJ_HASJIT
  expect_clocks_same(g, before);
#endif

  /* Unknown strings still create their ordinary numeric registry entry. */
#if LJ_HASJIT
  snapshot_clocks(g, before);
#endif
  lj_tab_keyed_slot_test_retry_stack_grow_once();
  attach_string_ok(L, tabref, fn_a, "unknown-event", 13, 0, 0x34u);
  assert(lj_tab_keyed_slot_test_retry_stack_grow_hits() == 1u);
  expect_raw_ref_int(L, event_key("unknown-event"), fn_a);
#if LJ_HASJIT
  expect_clocks_same(g, before);
#endif

  /* lj_lib_optstr retains stock numeric-to-string formatting, including
  ** signed zero and the canonical spellings of non-finite values. */
  for (i = 0; i < sizeof(numbers)/sizeof(numbers[0]); i++) {
#if LJ_HASJIT
    snapshot_clocks(g, before);
#endif
    attach_number_ok(L, tabref, fn_a, number_from_bits(numbers[i].bits),
		     numbers[i].formatted, (uint8_t)(0x35u + i));
    expect_raw_ref_int(L, event_key(numbers[i].formatted), fn_a);
#if LJ_HASJIT
    expect_clocks_same(g, before);
#endif
  }

#if LJ_HASJIT
  /* Runtime jit.off changes recording policy, not publication-clock policy. */
  ljt_lua_dostring(L, "jit.off(); assert(jit.status() == false)");
  {
    ClockState clock0 = clock_state(g, 2);
    attach_string_ok(L, tabref, fn_a, "record", 6, 0, 0x3bu);
    expect_clock_delta(g, 2, clock0, 1);
  }
#endif

  /* Extra arguments are ignored, including on a mapped lane. */
#if LJ_HASJIT
  {
    ClockState clock0 = clock_state(g, 0);
#endif
    attach_string_ok(L, tabref, fn_b, "bc", 2, 1, 0x36u);
    expect_raw_ref_int(L, VMEVENT_HASH(LJ_VMEVENT_BC), fn_b);
#if LJ_HASJIT
    expect_clock_delta(g, 0, clock0, 1);
  }
#endif

  /* Exact identity removal deletes fn_a entries but keeps both fn_b entries. */
#if LJ_HASJIT
  snapshot_clocks(g, before);
#endif
  memset(&attach_hook, 0, sizeof(attach_hook));
  attach_hook.L = L;
  attach_hook.g = g;
  attach_hook.table = event_table_ptr(L, tabref);
  attach_hook.fn = function_ref_ptr(L, fn_a);
  attach_hook.clean = clean_state(L);
  attach_hook.mode = ATTACH_HOOK_DETACH_SET;
  /* This broad detach has keys outside the dedicated shape set, so do not use
  ** its exact post-CAS classifier. The shape test below pins every callback. */
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  attach_hook.mode = ATTACH_HOOK_NONE;
  detach_ok(L, fn_a);
  expect_clean_state(L, attach_hook.clean);
  expect_raw_ref_int(L, VMEVENT_HASH(LJ_VMEVENT_BC), fn_b);
  expect_raw_ref_int(L, VMEVENT_HASH(LJ_VMEVENT_TEXIT), fn_b);
  expect_raw_nil_int(L, VMEVENT_HASH(LJ_VMEVENT_TRACE));
  expect_raw_nil_int(L, VMEVENT_HASH(LJ_VMEVENT_RECORD));
  expect_raw_nil_int(L, VMEVENT_HASH(LJ_VMEVENT_ERRFIN));
  expect_raw_nil_int(L, event_key_len(embedded_nul, 7));
  expect_raw_nil_int(L, event_key("unknown-event"));
  for (i = 0; i < sizeof(numbers)/sizeof(numbers[0]); i++)
    expect_raw_nil_int(L, event_key(numbers[i].formatted));
#if LJ_HASJIT
  expect_clock_delta(g, 1, before[1], 1);
  expect_clock_delta(g, 2, before[2], 1);
  expect_clock_delta(g, 4, before[4], 1);
#endif
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
  assert(event_metatable_hits == 0);

  /* Explicit nil is the optional-string detach spelling. */
#if LJ_HASJIT
  snapshot_clocks(g, before);
#endif
  {
    int top = lua_gettop(L);
    push_jit_attach(L);
    push_ref(L, fn_b);
    lua_pushnil(L);
    assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
    assert(lua_gettop(L) == top);
  }
  expect_raw_nil_int(L, VMEVENT_HASH(LJ_VMEVENT_BC));
  expect_raw_nil_int(L, VMEVENT_HASH(LJ_VMEVENT_TEXIT));
#if LJ_HASJIT
  expect_clock_delta(g, 0, before[0], 1);
  expect_clock_delta(g, 3, before[3], 1);
#endif
  assert(event_metatable_hits == 0);

  luaL_unref(L, LUA_REGISTRYINDEX, tabref);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_a);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_b);
  lua_close(L);
}

static void test_registry_diversion_captures_orphan(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  int fn_a = new_function_ref(L, attach_fn_a);
  int32_t key = event_key("trace");
  uint32_t slot = LJ_JIT_EVENT_ATTACHMENT_SLOT_NONE;
  CleanState clean;
#if LJ_HASJIT
  ClockState before;
#endif

  raw_clear_event_registry(L);
  expect_event_registry_nil(L);
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  lua_newtable(L);
  lua_pushcfunction(L, registry_divert_newindex);
  lua_setfield(L, -2, "__newindex");
  assert(lua_setmetatable(L, -2));
  lua_pop(L, 1);

  diverted_event_table = NULL;
  diverted_event_ref = LUA_NOREF;
  registry_diversion_hits = 0;
  memset(&attach_hook, 0, sizeof(attach_hook));
  attach_hook.L = L;
  attach_hook.g = g;
  attach_hook.fn = function_ref_ptr(L, fn_a);
  attach_hook.clean = clean_state(L);
  attach_hook.mode = ATTACH_HOOK_SINGLE;
  attach_hook.key = key;
  attach_hook.mask_before = 0x47u;
  (void)lj_jit_event_attachment_clock_slot(key, &slot);
  attach_hook.slot = slot;
#if LJ_HASJIT
  snapshot_clocks(g, attach_hook.clocks);
  before = attach_hook.clocks[slot];
#endif
  clean = attach_hook.clean;
  vmevmask_store_rel(g, attach_hook.mask_before);
  lj_tab_keyed_store_test_set_post_cas_hook(attach_post_cas_hook);
  assert(attach_string_status(L, fn_a, "trace", 5, 0) == LUA_OK);
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  assert(attach_hook.hits == 1);
  attach_hook.mode = ATTACH_HOOK_NONE;
  expect_clean_state(L, clean);

  assert(registry_diversion_hits == 1);
  assert(diverted_event_ref != LUA_NOREF && diverted_event_table != NULL);
  expect_event_registry_nil(L);
  expect_raw_ref_key(L, diverted_event_ref, push_key_integer,
	     (void *)(intptr_t)key, fn_a);
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
#if LJ_HASJIT
  expect_clock_delta(g, slot, before, 1);
#endif

  luaL_unref(L, LUA_REGISTRYINDEX, diverted_event_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_a);
  diverted_event_ref = LUA_NOREF;
  diverted_event_table = NULL;
  lua_close(L);
}

static void test_committed_stale_reconfirms_and_republishes(void)
{
  int32_t key = VMEVENT_HASH(LJ_VMEVENT_BC);
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  int fn_a = new_function_ref(L, attach_fn_a);
  int fn_b = new_function_ref(L, attach_fn_b);
  int tabref;
  GCtab *table;
  TValue *oldarray, *newarray;
  TValue oldsnap, newsnap;
  MSize oldasize, newasize, oldacap;
#if LJ_HASJIT
  ClockState before;
#endif

  assert(key > 0);
  lua_createtable(L, key + 64, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
  tabref = luaL_ref(L, LUA_REGISTRYINDEX);
  table = event_table_ptr(L, tabref);
  assert(lj_tab_array_separated(table));
  raw_set_ref(L, tabref, fn_a, push_key_integer, (void *)(intptr_t)key);

  oldarray = lj_tab_array_acq(table);
  oldasize = lj_tab_asize_acq(table);
  oldacap = lj_tab_acap_acq(table);
  assert(oldarray != NULL && (MSize)key < oldasize);
  lj_tab_resize(L, table, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(table);
  newasize = lj_tab_asize_acq(table);
  assert(newarray != oldarray && (MSize)key < newasize);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  /* The successor deliberately lacks the just-about-to-be-published value.
  ** First commit lands in the coherent old generation and returns STALE;
  ** fresh confirmation must then perform a second publication into this nil. */
  lj_tab_storenilraw(&newarray[key]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(table, oldasize);
  lj_tab_array_rel(table, oldarray);

  memset(&stale_hook, 0, sizeof(stale_hook));
  stale_hook.L = L;
  stale_hook.g = g;
  stale_hook.table = table;
  stale_hook.oldfn = function_ref_ptr(L, fn_a);
  stale_hook.newfn = function_ref_ptr(L, fn_b);
  stale_hook.oldarray = oldarray;
  stale_hook.newarray = newarray;
  stale_hook.newasize = newasize;
  stale_hook.clean = clean_state(L);
  stale_hook.mask_before = 0x61u;
#if LJ_HASJIT
  stale_hook.clock_before = clock_state(g, 0);
  before = stale_hook.clock_before;
#endif
  vmevmask_store_rel(g, stale_hook.mask_before);
  lj_tab_keyed_store_test_set_post_cas_hook(attach_committed_stale_hook);
  assert(attach_string_status(L, fn_b, "bc", 2, 0) == LUA_OK);
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  assert(stale_hook.hits == 2);
  expect_clean_state(L, stale_hook.clean);
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
#if LJ_HASJIT
  expect_clock_delta(g, 0, before, 2);
#endif
  lj_tv_load_acq(&oldsnap, &oldarray[key]);
  lj_tv_load_acq(&newsnap, &newarray[key]);
  assert(tvisfunc(&oldsnap) && funcV(&oldsnap) == stale_hook.newfn);
  assert(tvisfunc(&newsnap) && funcV(&newsnap) == stale_hook.newfn);
  expect_raw_ref_int(L, key, fn_b);

  /* Return the synthetic handoff to its ordinary retired shape. */
  lj_tab_array_rel(table, newarray);
  lj_tab_asize_rel(table, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  memset(&stale_hook, 0, sizeof(stale_hook));
  luaL_unref(L, LUA_REGISTRYINDEX, tabref);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_a);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_b);
  lua_close(L);
}

static void test_detach_key_shapes(void)
{
  lua_Number trace_number = (lua_Number)VMEVENT_HASH(LJ_VMEVENT_TRACE);
  lua_Number neg_double = -17.5;
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  int fn_a = new_function_ref(L, attach_fn_a);
  int fn_b = new_function_ref(L, attach_fn_b);
  int keyref;
  int udataref;
  int tabref;
#if LJ_HASJIT
  ClockState before[LJ_JIT_EVENT_ATTACHMENT_SLOTS];
#endif

  event_metatable_hits = 0;
  tabref = install_event_table(L, 1);
  lua_newtable(L);
  detach_table_key = tabV(L->top-1);
  keyref = luaL_ref(L, LUA_REGISTRYINDEX);
  detach_function_key = function_ref_ptr(L, fn_b);
  (void)lua_newuserdata(L, 24);
  detach_userdata_key = udataV(L->top-1);
  udataref = luaL_ref(L, LUA_REGISTRYINDEX);

  raw_set_ref(L, tabref, fn_a, push_key_integer,
	      (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_BC));
  raw_set_ref(L, tabref, fn_a, push_key_number, &trace_number);
  raw_set_ref(L, tabref, fn_a, push_key_integer,
	      (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_RECORD));
  raw_set_ref(L, tabref, fn_a, push_key_integer, (void *)(intptr_t)7);
  raw_set_ref(L, tabref, fn_a, push_key_number, &neg_double);
  raw_set_ref(L, tabref, fn_a, push_key_integer, (void *)(intptr_t)-19);
  raw_set_ref(L, tabref, fn_a, push_key_string, (void *)"shape-string");
  raw_set_ref(L, tabref, fn_a, push_key_bool, (void *)(intptr_t)1);
  raw_set_ref(L, tabref, fn_a, push_key_ref, (void *)(intptr_t)keyref);
  raw_set_ref(L, tabref, fn_a, push_key_lightud, &light_key_tag);
  raw_set_ref(L, tabref, fn_a, push_key_ref, (void *)(intptr_t)fn_b);
  raw_set_ref(L, tabref, fn_a, push_key_ref, (void *)(intptr_t)udataref);

  raw_set_ref(L, tabref, fn_b, push_key_string, (void *)"keep-other-fn");
  raw_set_ref(L, tabref, fn_b, push_key_integer,
	      (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_TEXIT));
  seed_nonfunction(L, tabref, "keep-nonfunction", 913);

#if LJ_HASJIT
  snapshot_clocks(g, before);
#endif
  memset(&attach_hook, 0, sizeof(attach_hook));
  attach_hook.L = L;
  attach_hook.g = g;
  attach_hook.table = event_table_ptr(L, tabref);
  attach_hook.fn = function_ref_ptr(L, fn_a);
  attach_hook.clean = clean_state(L);
  attach_hook.mode = ATTACH_HOOK_DETACH_SET;
  attach_hook.mask_before = 0x2au;
  vmevmask_store_rel(g, attach_hook.mask_before);
  lj_tab_keyed_store_test_set_post_cas_hook(attach_post_cas_hook);
  detach_ok(L, fn_a);
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  assert(attach_hook.hits == 12);
  assert(attach_hook.detach_seen == DETACH_SEEN_ALL);
  attach_hook.mode = ATTACH_HOOK_NONE;
  expect_clean_state(L, attach_hook.clean);

  expect_raw_nil_key(L, tabref, push_key_integer,
	     (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_BC));
  expect_raw_nil_key(L, tabref, push_key_number, &trace_number);
  expect_raw_nil_key(L, tabref, push_key_integer,
	     (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_RECORD));
  expect_raw_nil_key(L, tabref, push_key_integer, (void *)(intptr_t)7);
  expect_raw_nil_key(L, tabref, push_key_number, &neg_double);
  expect_raw_nil_key(L, tabref, push_key_integer, (void *)(intptr_t)-19);
  expect_raw_nil_key(L, tabref, push_key_string, (void *)"shape-string");
  expect_raw_nil_key(L, tabref, push_key_bool, (void *)(intptr_t)1);
  expect_raw_nil_key(L, tabref, push_key_ref, (void *)(intptr_t)keyref);
  expect_raw_nil_key(L, tabref, push_key_lightud, &light_key_tag);
  expect_raw_nil_key(L, tabref, push_key_ref, (void *)(intptr_t)fn_b);
  expect_raw_nil_key(L, tabref, push_key_ref, (void *)(intptr_t)udataref);

  expect_raw_ref_key(L, tabref, push_key_string,
	     (void *)"keep-other-fn", fn_b);
  expect_raw_ref_key(L, tabref, push_key_integer,
	     (void *)(intptr_t)VMEVENT_HASH(LJ_VMEVENT_TEXIT), fn_b);
  expect_nonfunction(L, tabref, "keep-nonfunction", 913);
#if LJ_HASJIT
  expect_clock_delta(g, 0, before[0], 1);
  expect_clock_delta(g, 1, before[1], 1);
  expect_clock_delta(g, 2, before[2], 1);
  expect_clock_delta(g, 3, before[3], 0);
  expect_clock_delta(g, 4, before[4], 0);
  assert(vmevmask_load_acq(g) == VMEVENT_NOCACHE);
#else
  /* With no JIT every detach is the stock unclocked path and does not
  ** explicitly invalidate the VM-event missing-handler cache. */
  assert(vmevmask_load_acq(g) == attach_hook.mask_before);
#endif
  assert(event_metatable_hits == 0);

  /* An arbitrary-key-only detach never invalidates, even in a JIT build. */
  luaL_unref(L, LUA_REGISTRYINDEX, tabref);
  tabref = install_event_table(L, 1);
  raw_set_ref(L, tabref, fn_a, push_key_string,
	      (void *)"unclocked-detach-only");
  vmevmask_store_rel(g, 0x29u);
  detach_ok(L, fn_a);
  assert(vmevmask_load_acq(g) == 0x29u);
  expect_raw_nil_key(L, tabref, push_key_string,
	     (void *)"unclocked-detach-only");

  luaL_unref(L, LUA_REGISTRYINDEX, keyref);
  luaL_unref(L, LUA_REGISTRYINDEX, udataref);
  luaL_unref(L, LUA_REGISTRYINDEX, tabref);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_a);
  luaL_unref(L, LUA_REGISTRYINDEX, fn_b);
  detach_table_key = NULL;
  detach_function_key = NULL;
  detach_userdata_key = NULL;
  lua_close(L);
}

static void expect_error_contains(lua_State *L, int status,
			  int base, const char *needle)
{
  const char *err;
  assert(status != LUA_OK);
  assert(lua_gettop(L) == base + 1);
  err = lua_tostring(L, -1);
  assert(err != NULL && strstr(err, needle) != NULL);
  lua_pop(L, 1);
  assert(lua_gettop(L) == base);
}

static void test_argument_order_and_registry_conflict(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  int fn_a = new_function_ref(L, attach_fn_a);
  CleanState clean = clean_state(L);
  int top, status;
#if LJ_HASJIT
  ClockState unchanged[LJ_JIT_EVENT_ATTACHMENT_SLOTS];
#endif

  lua_pushboolean(L, 0);
  lua_setfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);

  top = lua_gettop(L);
  push_jit_attach(L);
  lua_pushboolean(L, 0);
  lua_newtable(L);
  status = lua_pcall(L, 2, 0, 0);
  expect_error_contains(L, status, top, "bad argument #1");
  expect_clean_state(L, clean);

  top = lua_gettop(L);
  push_jit_attach(L);
  push_ref(L, fn_a);
  lua_newtable(L);
  status = lua_pcall(L, 2, 0, 0);
  expect_error_contains(L, status, top, "bad argument #2");
  expect_clean_state(L, clean);

  top = lua_gettop(L);
  push_jit_attach(L);
  push_ref(L, fn_a);
  lua_pushboolean(L, 0);
  status = lua_pcall(L, 2, 0, 0);
  expect_error_contains(L, status, top, "bad argument #2");
  expect_clean_state(L, clean);

#if LJ_HASJIT
  snapshot_clocks(g, unchanged);
#endif
  vmevmask_store_rel(g, 0x51u);
  top = lua_gettop(L);
  status = attach_string_status(L, fn_a, "trace", 5, 0);
  expect_error_contains(L, status, top,
			"name conflict for module '_VMEVENTS'");
  expect_clean_state(L, clean);
  assert(vmevmask_load_acq(g) == 0x51u);
#if LJ_HASJIT
  expect_clocks_same(g, unchanged);
  snapshot_clocks(g, unchanged);
#endif

  vmevmask_store_rel(g, 0x52u);
  top = lua_gettop(L);
  push_jit_attach(L);
  push_ref(L, fn_a);
  status = lua_pcall(L, 1, 0, 0);
  expect_error_contains(L, status, top,
			"name conflict for module '_VMEVENTS'");
  expect_clean_state(L, clean);
  assert(vmevmask_load_acq(g) == 0x52u);
#if LJ_HASJIT
  expect_clocks_same(g, unchanged);
#endif

  /* Missing function validation still precedes the registry lookup. */
  top = lua_gettop(L);
  push_jit_attach(L);
  status = lua_pcall(L, 0, 0, 0);
  expect_error_contains(L, status, top, "bad argument #1");
  expect_clean_state(L, clean);

  luaL_unref(L, LUA_REGISTRYINDEX, fn_a);
  lua_close(L);
}

static uint32_t close_probe_hits;
#if LJ_HASJIT
static ClockState close_trace_before;
#endif

static int close_probe(lua_State *L)
{
  int stage = luaL_checkint(L, 1);
  assert(mt_shutdown_acq(G(L)) != 0);
  assert(L == mainthread_acq(G(L)) && L2TG(L) == G(L)->main_tg);
  assert(gc2_smr_readers_acq(G(L)) == 0);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
#if LJ_HASJIT
  expect_clock_delta(G(L), 1, close_trace_before, (uint64_t)stage);
#endif
  assert(vmevmask_load_acq(G(L)) == VMEVENT_NOCACHE);
  assert(stage == (int)close_probe_hits + 1);
  close_probe_hits++;
  return 0;
}

static void test_real_close_finalizer(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  close_probe_hits = 0;
#if LJ_HASJIT
  close_trace_before = clock_state(G(L), 1);
#endif
  lua_pushcfunction(L, close_probe);
  lua_setglobal(L, "lj_attach_close_probe");
  ljt_lua_dostring(L,
    "local attach = jit.attach\n"
    "local fn = function() end\n"
    "local probe = lj_attach_close_probe\n"
    "local p = newproxy(true)\n"
    "getmetatable(p).__gc = function()\n"
    "  attach(fn, 'trace')\n"
    "  probe(1)\n"
    "  attach(fn)\n"
    "  probe(2)\n"
    "end\n"
    "_G.lj_attach_close_proxy = p\n");
  lua_close(L);
  assert(close_probe_hits == 2);
}

#else  /* LUAJIT_DISABLE_VMEVENT */

static void expect_disabled_error(lua_State *L, int nargs)
{
  int top = lua_gettop(L) - nargs - 1;
  int status = lua_pcall(L, nargs, 0, 0);
  const char *err;
  assert(status != LUA_OK && lua_gettop(L) == top + 1);
  err = lua_tostring(L, -1);
  assert(err != NULL && strstr(err, "vmevent API disabled") != NULL);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
}

static void test_disabled_vmevent_order(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_getfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  /* Disabled builds reject before any ordinary argument validation. */
  push_jit_attach(L);
  expect_disabled_error(L, 0);
  push_jit_attach(L);
  lua_pushboolean(L, 0);
  lua_newtable(L);
  expect_disabled_error(L, 2);
  push_jit_attach(L);
  lua_pushcfunction(L, attach_fn_a);
  lua_pushliteral(L, "trace");
  lua_pushliteral(L, "ignored");
  expect_disabled_error(L, 3);

  lua_getfield(L, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_close(L);
}

#endif /* LUAJIT_DISABLE_VMEVENT */

int main(void)
{
  test_event_hashes();
#ifdef LUAJIT_DISABLE_VMEVENT
  test_disabled_vmevent_order();
#else
  test_public_attach_and_hash_compatibility();
  test_registry_diversion_captures_orphan();
  test_committed_stale_reconfirms_and_republishes();
  test_detach_key_shapes();
  test_argument_order_and_registry_conflict();
  test_real_close_finalizer();
#endif
  puts("t-jit-attach-clocked OK");
  return 0;
}
