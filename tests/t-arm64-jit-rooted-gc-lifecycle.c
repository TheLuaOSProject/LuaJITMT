/* Native GC-valued rooted table operations retain results across helpers and
** collection, and restore the same result after the native lease ends. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_TAB_TEST_HELPERS)

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_trace.h"

typedef struct ReadStats {
  uint32_t hits;
  uint32_t tables;
  uint32_t strings;
  uint32_t functions;
  uint32_t threads;
  uint32_t userdata;
  uint32_t cdata;
} ReadStats;

static ReadStats stats;
static TGState *lease_tg;
static GCobj *lease_target;
static GCobj *lease_pause_target;
static uint32_t pause_armed;
static uint32_t pause_seen;
static uint32_t release_native;
static uint32_t phase_attempted;
static uint32_t native_target_stores;

static void observe_rooted_gc_read(lua_State *L, cTValue *outroot)
{
  TGState *tg = L2TG(L);
  TValue snap;
  assert(tg != NULL);
  /* The shared helper also serves interpreter/meta paths. Count only the
  ** calls which prove execution under a published native lease. */
  if (lj_tg_load_jit_base(tg) == NULL) return;
  lj_tv_load_acq(&snap, outroot);
  assert(tvisgcv(&snap));
  stats.hits++;
  if (tvistab(&snap)) stats.tables++;
  else if (tvisstr(&snap)) stats.strings++;
  else if (tvisfunc(&snap)) stats.functions++;
  else if (tvisthread(&snap)) stats.threads++;
  else if (tvisudata(&snap)) stats.userdata++;
  else if (tviscdata(&snap)) stats.cdata++;
  if (lease_tg && la_load32_acq(&pause_armed) && tvistab(&snap) &&
      gcV(&snap) == lease_pause_target) {
    uint32_t expect = 1;
    if (la_cas32(&pause_armed, &expect, 0,
                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      uint32_t spins;
      la_store32_rel(&pause_seen, 1);
      for (spins = 0; spins < 50000000u &&
           la_load32_acq(&release_native) == 0; spins++)
        (void)sched_yield();
      assert(la_load32_acq(&release_native) == 1);
      assert(la_load32_acq(&phase_attempted) == 1);
    }
  }
}

static void observe_rooted_gc_store(lua_State *L, TValue *tabroot,
                                    TValue *keyroot, TValue *valueroot)
{
  UNUSED(tabroot); UNUSED(keyroot);
  if (!lease_tg || L2TG(L) != lease_tg ||
      lj_tg_load_jit_base(lease_tg) == NULL)
    return;
  if (tvisgcv(valueroot) && gcV(valueroot) == lease_target)
    la_add32_rlx(&native_target_stores, 1);
}

static int gc_pause_seen_fn(lua_State *L)
{
  UNUSED(L);
  lua_pushboolean(L, la_load32_acq(&pause_seen) != 0);
  return 1;
}

static int gc_release_native_fn(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&release_native, 1);
  return 0;
}

/* Request a major cycle from the peer while the trace is paused after the
** target read and a later rooted read has overwritten the helper scratch. */
static int gc_attempt_phase_fn(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  lj_gc2_force_major(g);
  assert(tg != NULL && gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_gc2_request_cycle_explicit(g, tg));
  /* mark_begin closes the native gate first. With the target trace still
  ** published, its active-jit check returns before physical phase entry. */
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) == 0);
  assert(!lj_gc2_jit_entry_open(g));
  assert(lease_tg != NULL && lj_tg_load_jit_base(lease_tg) != NULL);
  la_store32_rel(&phase_attempted, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static void run_file(lua_State *L, const char *path)
{
  int status = luaL_dofile(L, path);
  if (status != LUA_OK) {
    fprintf(stderr, "rooted GC lifecycle fixture: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static void drain_gc_to_idle(lua_State *L)
{
  global_State *g = G(L);
  uint32_t i;
  for (i = 0; i < 2000000u && gc2_phase_acq(g) != LJ_GC2_IDLE; i++)
    (void)lua_gc(L, LUA_GCSTEP, 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static lua_Integer call_kernel(lua_State *L, lua_Integer n)
{
  int status;
  lua_getglobal(L, "__arm64_rooted_gc_kernel");
  lua_getglobal(L, "__arm64_rooted_gc_root");
  lua_pushinteger(L, n);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "rooted GC kernel failed: %s\n", lua_tostring(L, -1));
    abort();
  }
  assert(lua_isnumber(L, -1));
  n = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return n;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return funcproto(fn);
}

static void test_live_register_gc_handoff(void)
{
  lua_State *L = luaL_newstate();
  uint32_t i;
  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, gc_pause_seen_fn);
  lua_setglobal(L, "__gc_rooted_native_paused");
  lua_pushcfunction(L, gc_attempt_phase_fn);
  lua_setglobal(L, "__gc_rooted_attempt_phase");
  lua_pushcfunction(L, gc_release_native_fn);
  lua_setglobal(L, "__gc_rooted_release_native");
  assert(luaL_dostring(L,
    "package.path='./src/?.lua;./src/?/init.lua;'..package.path\n"
    "collectgarbage('stop')\n"
    "local th=require('threading')\n"
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2')\n"
    "local target={}\n"
    "local weak=setmetatable({target},{__mode='v'})\n"
    "__arm64_rooted_gc_root={src={payload=target},dst={payload=target},\n"
    " noise={payload={step=1}},key='payload',weak=weak}\n"
    "target=nil\n"
    "function __arm64_rooted_gc_kernel(root,n)\n"
    " local src,key=root.src,root.key; local i=1\n"
    " while i<=n do\n"
    "  local value=src[key]\n"
    "  local noise=root.noise[key]\n"
    "  root.dst[key]=value\n"
    "  i=i+noise.step\n"
    " end\n"
    " return i-1\n"
    "end\n"
    "local ready=th.channel(1)\n"
    "__arm64_rooted_gc_peer=th.spawn(function(root,rdy)\n"
    " assert(rdy:send(true,5))\n"
    " for i=1,30000 do\n"
    "  if __gc_rooted_native_paused() then break end\n"
    "  th.sleep(0.001)\n"
    " end\n"
    " assert(__gc_rooted_native_paused(),'native read hook timeout')\n"
    " root.src[root.key]=nil\n"
    " assert(__gc_rooted_attempt_phase(), 'collector did not defer at jit_base')\n"
    " __gc_rooted_release_native()\n"
    " return true\n"
    "end,__arm64_rooted_gc_root,ready)\n"
    "local _,ok=ready:recv(5); assert(ok)\n") == LUA_OK);
  lease_tg = L2TG(L);
  assert(lease_tg != NULL);
  lua_getglobal(L, "__arm64_rooted_gc_root");
  lua_getfield(L, -1, "src");
  lua_getfield(L, -1, "payload");
  assert(tvistab(L->top - 1));
  lease_target = gcV(L->top - 1); /* Identity only; deliberately not a root. */
  lua_pop(L, 3);
  lua_getglobal(L, "__arm64_rooted_gc_root");
  lua_getfield(L, -1, "noise");
  lua_getfield(L, -1, "payload");
  assert(tvistab(L->top - 1));
  lease_pause_target = gcV(L->top - 1);
  lua_pop(L, 3);

  for (i = 0; i < 20; i++) assert(call_kernel(L, 64) == 64);
  assert(luaL_dostring(L,
    "__arm64_rooted_gc_root.dst.payload=false\n"
    "collectgarbage('collect')\n") == LUA_OK);
  drain_gc_to_idle(L);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  assert(proto_trace_acq(global_proto(L, "__arm64_rooted_gc_kernel")) != 0);
  setnilV(&lease_tg->tmptv3); /* Do not let warmup scratch root the target. */
  la_store32_rel(&pause_armed, 1);
  la_store32_rel(&pause_seen, 0);
  la_store32_rel(&release_native, 0);
  la_store32_rel(&phase_attempted, 0);
  la_store32_rel(&native_target_stores, 0);
  lj_tab_test_set_rooted_gc_read_hook(observe_rooted_gc_read);
  lj_tab_test_set_rooted_gc_store_hook(observe_rooted_gc_store);

  /* Pause on the second GC read: the target has already been loaded into a
  ** live native value and this helper has replaced the target scratch root. */
  assert(call_kernel(L, 1) == 1);
  assert(la_load32_acq(&pause_seen) == 1);
  assert(la_load32_acq(&phase_attempted) == 1);
  assert(la_load32_acq(&native_target_stores) == 1);
  assert(lj_tg_load_jit_base(lease_tg) == NULL);
  assert(luaL_dostring(L,
    "local ok,result=__arm64_rooted_gc_peer:join(5)\n"
    "assert(ok and result)\n"
    "local r=__arm64_rooted_gc_root\n"
    "assert(r.src.payload==nil)\n"
    "assert(r.dst.payload==r.weak[1])\n"
    "collectgarbage('collect')\n"
    "assert(r.dst.payload==r.weak[1])\n") == LUA_OK);
  lua_getglobal(L, "__arm64_rooted_gc_root");
  lua_getfield(L, -1, "dst");
  lua_getfield(L, -1, "payload");
  assert(tvistab(L->top - 1) && gcV(L->top - 1) == lease_target);
  lua_pop(L, 3);

  /* Reinsert the retained object and run the same native trace after the GC
  ** handoff. Finally drop both strong edges and prove the weak witness clears. */
  {
    uint32_t before = stats.hits;
    assert(luaL_dostring(L,
      "local r=__arm64_rooted_gc_root\n"
      "r.src.payload=r.dst.payload\n") == LUA_OK);
    assert(call_kernel(L, 64) == 64);
    assert(stats.hits > before);
    assert(luaL_dostring(L,
      "local r=__arm64_rooted_gc_root\n"
      "assert(r.dst.payload==r.weak[1])\n"
      "r.src.payload=nil; r.dst.payload=nil\n"
      "collectgarbage('collect')\n"
      "assert(r.weak[1]==nil,'unreachable result was retained')\n") == LUA_OK);
  }
  assert(lj_tg_load_jit_base(lease_tg) == NULL);
  lj_tab_test_set_rooted_gc_read_hook(NULL);
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  lua_close(L);
  lease_tg = NULL;
  lease_target = NULL;
  lease_pause_target = NULL;
}

int main(int argc, char **argv)
{
  lua_State *L;
  TGState *tg;
  uint32_t anchors, reads_before;
  uint32_t depth;
  int status;
  assert(argc == 2);
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "package.path = './src/?.lua;./src/?/init.lua;' .. package.path") == LUA_OK);
  tg = L2TG(L);
  anchors = lj_tg_root_anchor_top_acq(tg);
  depth = lj_tg_tab_read_depth_acq(tg);
  lj_tab_test_set_rooted_gc_read_hook(observe_rooted_gc_read);

  run_file(L, argv[1]);
  assert(stats.hits > 100);
  assert(stats.tables > 0 && stats.strings > 0 && stats.functions > 0 &&
         stats.threads > 0 && stats.userdata > 0 && stats.cdata > 0);
  assert(lj_tg_root_anchor_top_acq(tg) == anchors);
  assert(lj_tg_tab_read_depth_acq(tg) == depth);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(lj_tg_load_jit_base(tg) == NULL);

  /* Run the same compiled loop after the fixture's full collections. The
  ** read hook's native-lease assertion and counter delta prove re-entry. */
  reads_before = stats.hits;
  lua_getglobal(L, "__arm64_rooted_gc_reentry");
  assert(lua_isfunction(L, -1));
  status = lua_pcall(L, 0, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "rooted GC reentry failed: %s\n", lua_tostring(L, -1));
    abort();
  }
  assert(lua_tointeger(L, -1) == 32);
  lua_pop(L, 1);
  assert(stats.hits > reads_before);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_root_anchor_top_acq(tg) == anchors);
  assert(lj_tg_tab_read_depth_acq(tg) == depth);

  lj_tab_test_set_rooted_gc_read_hook(NULL);
  lua_close(L);
  test_live_register_gc_handoff();
  puts("t-arm64-jit-rooted-gc-lifecycle OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-rooted-gc-lifecycle SKIP");
  return 0;
}

#endif
