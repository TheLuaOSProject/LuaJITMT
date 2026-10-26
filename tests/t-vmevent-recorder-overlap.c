/* Deterministically overlap a legacy peer TEXIT callback with active recording. */

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "luajit.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_profile.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lj_vmevent.h"

#include "lib/test_sleep.h"

#ifndef LJ_TRACE_TEST_HELPERS
#error "t-vmevent-recorder-overlap requires LJ_TRACE_TEST_HELPERS"
#endif

#if LJ_HASJIT && (LJ_TARGET_X64 || LJ_TARGET_ARM64)

#define WAIT_ATTEMPTS 5000u
#define WAIT_NS 1000000L

static TGState *owner_tg;
static GCproto *owner_pt;
static const BCIns *owner_cget_pc;
static const BCIns *owner_call_pc;
static uint32_t cget_seen;
static uint32_t callback_active;
static uint32_t call_recorded_while_callback;
static uint32_t notify_seen;
static uint32_t callback_done;
static uint32_t callback_entries;
static uint32_t recursive_event_returned;
static uint32_t global_vmevent_seen;
static uint32_t local_vmevent_seen;
static uint32_t event_stack_mismatch;
static uint32_t wait_timeout;
static int use_profile;

static int wait_for(const uint32_t *flag)
{
  uint32_t i;
  for (i = 0; i < WAIT_ATTEMPTS; i++) {
    if (la_load32_acq(flag) != 0)
      return 1;
    sleep_ns(WAIT_NS);
  }
  return 0;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void recorder_hook(jit_State *J, const BCIns *pc)
{
  if (J->pt != owner_pt)
    return;
  if (pc == owner_cget_pc) {
    la_store32_rel(&cget_seen, 1);
    if (!wait_for(&callback_active))
      la_store32_rel(&wait_timeout, 1);
  } else if (pc == owner_call_pc &&
             la_load32_acq(&callback_active) != 0 &&
             la_load32_acq(&callback_done) == 0) {
    la_add32_rlx(&call_recorded_while_callback, 1);
  }
}

/* Trigger the real legacy TEXIT prepare/call path from the peer TG. */
static int peer_send_texit(lua_State *L)
{
  ptrdiff_t top = savestack(L, L->top);
  if (use_profile) {
    assert(raise(SIGPROF) == 0);
    lj_profile_owner_poll(L);
    lj_profile_interpreter(L);
  } else {
    lj_vmevent_send_l(L, TEXIT,
      setintV(V->top++, 0);
      setintV(V->top++, 0);
    );
  }
  if (savestack(L, L->top) != top)
    la_store32_rel(&event_stack_mismatch, 1);
  /* Publish only after the legacy event path has restored its masks. */
  la_store32_rel(&callback_done, 1);
  return 0;
}

/* Called from the Lua TEXIT handler. Keep it active until the callee's C
** notification proves the recorder crossed the caller's BC_CALL boundary. */
static void peer_hold_body(lua_State *L, int recursive)
{
  TGState *tg = L2TG(L);
  if (tg == owner_tg)
    return;  /* Ignore ordinary owner-side trace exits. */
  assert(((lj_tg_hookmask_load(tg) | hookmask_load(G(L))) &
          HOOK_VMEVENT) != 0);
  if (lj_tg_hookmask_load(tg) & HOOK_VMEVENT) {
    assert(lj_tg_hookmask_load(tg) & HOOK_ACTIVE);
    la_store32_rel(&local_vmevent_seen, 1);
  }
  if (hookmask_load(G(L)) & HOOK_VMEVENT)
    la_store32_rel(&global_vmevent_seen, 1);
  la_add32_rlx(&callback_entries, 1);
  la_store32_rel(&callback_active, 1);

  if (recursive) {
    /* Same-TG recursion is a required bounded drop, not a second callback. */
    lj_vmevent_send_l(L, TEXIT,
      setintV(V->top++, 0);
      setintV(V->top++, 0);
    );
    la_store32_rel(&recursive_event_returned, 1);
  }

  /* Keep this callback active until the owner crosses CALL and reaches the
  ** callee-side notification. */
  if (!wait_for(&notify_seen))
    la_store32_rel(&wait_timeout, 1);

}

static int peer_hold_callback(lua_State *L)
{
  peer_hold_body(L, 1);
  return 0;
}

static void peer_profile_callback(void *data, lua_State *L, int samples,
                                  int vmstate)
{
  UNUSED(data);
  UNUSED(vmstate);
  assert(samples > 0);
  assert(lj_profile_callback_active_tg(L2TG(L)));
  peer_hold_body(L, 0);
}

/* The callee reaches this only after the caller BC_CALL has executed. */
static int owner_notify(lua_State *L)
{
  UNUSED(L);
  if (la_load32_acq(&cget_seen) == 0)
    return 0;  /* Warmup executes this callee before recording starts. */
  la_store32_rel(&notify_seen, 1);
  if (!wait_for(&callback_done))
    la_store32_rel(&wait_timeout, 1);
  return 0;
}

static int owner_cget_seen(lua_State *L)
{
  lua_pushboolean(L, la_load32_acq(&cget_seen) != 0);
  return 1;
}

static void run_lua(lua_State *L, const char *source)
{
  int status = luaL_dostring(L, source);
  if (status != LUA_OK) {
    fprintf(stderr, "vmevent-recorder-overlap: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void find_owner_call_sites(GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  MSize i;
  owner_cget_pc = NULL;
  owner_call_pc = NULL;
  for (i = 0; i < pt->sizebc; i++) {
    if (bc_op(bc[i]) == BC_CALL) {
      MSize j = i;
      while (j > 0) {
        j--;
        if (bc_op(bc[j]) == BC_CGET && bc_a(bc[j]) == bc_a(bc[i])) {
          owner_cget_pc = &bc[j];
          owner_call_pc = &bc[i];
          break;
        }
      }
      if (owner_call_pc != NULL)
        break;
    }
  }
  assert(owner_cget_pc != NULL && owner_call_pc != NULL);
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  use_profile = argc > 1 && strcmp(argv[1], "profile") == 0;
  uint8_t owner_hookmask_before;
  uint8_t global_hookmask_before;
  int status;
  assert(L != NULL);
  luaL_openlibs(L);
  owner_tg = L2TG(L);

  lua_pushcfunction(L, peer_send_texit);
  lua_setglobal(L, "overlap_peer_send_texit");
  lua_pushcfunction(L, peer_hold_callback);
  lua_setglobal(L, "overlap_peer_hold_callback");
  lua_pushcfunction(L, owner_notify);
  lua_setglobal(L, "overlap_owner_notify");
  lua_pushcfunction(L, owner_cget_seen);
  lua_setglobal(L, "overlap_cget_seen");
  lua_pushboolean(L, use_profile);
  lua_setglobal(L, "overlap_profile_mode");

  run_lua(L,
    "local jit = require('jit')\n"
    "local threading = require('threading')\n"
    "overlap_table = {}\n"
    "overlap_callee = function()\n"
    "  overlap_owner_notify()\n"
    "  return overlap_table\n"
    "end\n"
    "jit.off(overlap_callee, true)\n"
    "overlap_owner = function(callee, n)\n"
    "    local i, result = 1, nil\n"
    "    while i <= n do\n"
    "      result = callee()\n"
    "      i = i + 1\n"
    "    end\n"
    "    return result\n"
    "end\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=8')\n"
    "jit.on(overlap_owner, true)\n"
    "if not overlap_profile_mode then\n"
    "  jit.attach(function() overlap_peer_hold_callback() end, 'texit')\n"
    "end\n"
    "local function peer()\n"
    "  while not overlap_cget_seen() do end\n"
    "  overlap_peer_send_texit()\n"
    "  return true\n"
    "end\n"
    "jit.off(peer, true)\n"
    "overlap_peer = assert(threading.spawn(peer))\n");

  owner_pt = global_proto(L, "overlap_owner");
  find_owner_call_sites(owner_pt);
  assert((hookmask_load(G(L)) & HOOK_VMEVENT) == 0);
  owner_hookmask_before = (uint8_t)lj_tg_hookmask_load(owner_tg);
  global_hookmask_before = (uint8_t)hookmask_load(G(L));
  if (use_profile)
    luaJIT_profile_start(L, "i1000000", peer_profile_callback, NULL);

  lj_trace_test_set_recorded_hook(recorder_hook);
  lua_getglobal(L, "overlap_owner");
  lua_getglobal(L, "overlap_callee");
  lua_pushinteger(L, 32);
  status = lua_pcall(L, 2, 1, 0);
  lj_trace_test_set_recorded_hook(NULL);
  if (status != LUA_OK) {
    fprintf(stderr, "owner loop failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  lua_getglobal(L, "overlap_table");
  assert(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
  run_lua(L,
    "local ok, result = overlap_peer:join(10)\n"
    "assert(ok and result == true)\n");
  if (use_profile)
    luaJIT_profile_stop(L);

  assert(la_load32_acq(&cget_seen) != 0);
  assert(la_load32_acq(&callback_active) != 0);
  assert(la_load32_acq(&call_recorded_while_callback) != 0);
  assert(la_load32_acq(&notify_seen) != 0);
  assert(la_load32_acq(&callback_done) != 0);
  assert(la_load32_acq(&callback_entries) == 1);
  if (!use_profile)
    assert(la_load32_acq(&recursive_event_returned) != 0);
  assert(la_load32_acq(&local_vmevent_seen) != 0);
  assert(la_load32_acq(&global_vmevent_seen) <= 1);
  assert(la_load32_acq(&event_stack_mismatch) == 0);
  assert(la_load32_acq(&wait_timeout) == 0);
  assert((uint8_t)lj_tg_hookmask_load(owner_tg) == owner_hookmask_before);
  assert((uint8_t)hookmask_load(G(L)) == global_hookmask_before);
  lua_close(L);
  puts("t-vmevent-recorder-overlap OK");
  return 0;
}

#else

int main(void)
{
  puts("t-vmevent-recorder-overlap skipped: JIT target unavailable");
  return 0;
}

#endif
