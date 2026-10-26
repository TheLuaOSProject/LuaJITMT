/* Native CALL continuation and effect-once coverage for rooted rawset. */
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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_TAB_TEST_HELPERS) && \
    defined(LJ_TG_ROOT_TEST_HELPERS)

#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_tab.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED
#error "rawset exit fixture requires the admitted ARM64 root loop"
#endif

static GCtab *target_table;
static uint32_t rooted_cas_calls;
static uint32_t target_cas_calls;
static uint32_t mutate_after_cas;
static uint32_t mutated_after_cas;
static uint32_t alternate_calls;
static uint32_t newindex_calls;
static uint32_t helper_throws;

static uint32_t prime_anchor_boundary(lua_State *L, uint32_t *savedp);
static void restore_anchor_boundary(lua_State *L, uint32_t saved);

static void count_store_cas(lua_State *L, GCtab *t, TValue *dst,
			    cTValue *key, cTValue *value)
{
  UNUSED(key);
  UNUSED(value);
  if (t != target_table)
    return;
  (void)la_add32_rlx(&target_cas_calls, 1);
  if (lj_tg_load_jit_base(L2TG(L)) == NULL) return;
  (void)la_add32_rlx(&rooted_cas_calls, 1);
  if (la_xchg32_acqrel(&mutate_after_cas, 0) != 0) {
    TValue boolv;
    setboolV(&boolv, 1);
    tv_rawstore_rel(dst, tv_rawload(&boolv));
    (void)la_add32_rlx(&mutated_after_cas, 1);
  }
}

static int alternate_rawset(lua_State *L)
{
  assert(lua_gettop(L) == 3);
  luaL_checktype(L, 1, LUA_TTABLE);
  (void)la_add32_rlx(&alternate_calls, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_rawset(L, 1);
  lua_pushvalue(L, 1);
  return 1;
}

static int throwing_newindex(lua_State *L)
{
  (void)la_add32_rlx(&newindex_calls, 1);
  return luaL_error(L, "rawset must bypass __newindex");
}

static void throw_before_store(lua_State *L, TValue *tab, TValue *key,
                               TValue *value)
{
  UNUSED(tab); UNUSED(key); UNUSED(value);
  if (lj_tg_load_jit_base(L2TG(L)) == NULL) return;
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  helper_throws++;
  (void)luaL_error(L, "rawset helper unwind");
}

static void run_chunk(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "rawset-exit setup failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void set_throwing_meta(lua_State *L, int table_index)
{
  lua_newtable(L);
  lua_pushcfunction(L, throwing_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, table_index);
}

static int call_kernel(lua_State *L, const char *name, int alternate,
		       lua_Integer base, lua_Number value, lua_Integer n,
		       int expect_error, int fail_anchor_reserve)
{
  int table_index, status;
  uint32_t saved = 0;
  lua_newtable(L);
  table_index = lua_gettop(L);
  set_throwing_meta(L, table_index);
  target_table = tabV(L->top - 1);

  lua_getglobal(L, "__arm64_rawset_invoke");
  lua_getglobal(L, name);
  if (alternate)
    lua_getglobal(L, "rawset_alt");
  else
    lua_getglobal(L, "rawset");
  lua_pushvalue(L, table_index);
  lua_pushinteger(L, base);
  lua_pushnumber(L, value);
  lua_pushinteger(L, n);
  /* Arm the reserve seam only after Lua stack/table setup is complete, so
  ** its single failure belongs to the native rooted helper. */
  if (fail_anchor_reserve) {
    (void)prime_anchor_boundary(L, &saved);
    lj_tg_root_test_fail_reserve_after(1);
  }
  status = lua_pcall(L, 6, 1, 0);
  if (fail_anchor_reserve) {
    lj_tg_root_test_fail_reserve_after(0);
    restore_anchor_boundary(L, saved);
  }
  if (expect_error) {
    const char *msg;
    assert(status == LUA_ERRRUN);
    msg = lua_tostring(L, -1);
    assert(msg && strstr(msg, expect_error == 2 ?
                         "rawset helper unwind" : "arithmetic") != NULL);
    lua_pop(L, 1);
  } else {
    assert(status == LUA_OK);
    assert(lua_isnumber(L, -1) && lua_tonumber(L, -1) == (lua_Number)n + 1.0);
    lua_pop(L, 1);
  }

  lua_pushinteger(L, base + 1);
  lua_rawget(L, table_index);
  if (expect_error == 2) {
    assert(lua_isnil(L, -1));
  } else if (expect_error) {
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1));
  } else {
    assert(lua_isnumber(L, -1) && lua_tonumber(L, -1) == value);
  }
  lua_pop(L, 1);
  target_table = NULL;
  lua_settop(L, table_index - 1);
  return status;
}

static GCproto *get_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return funcproto(fn);
}

static GCtrace *assert_trace(lua_State *L, const char *name, BCOp loopop,
			     uint32_t cargc)
{
  jit_State *J = L2J(L);
  GCproto *pt = get_proto(L, name);
  TraceNo traceno = proto_trace_acq(pt);
  GCtrace *T;
  const IRIns *ir;
  IRRef r;
  uint32_t calls = 0;
  if (!traceno) fprintf(stderr, "missing rawset trace: %s\n", name);
  assert(traceno != 0);
  T = traceref_safe(J, traceno);
  assert(T && trace_runnable_acq(T, traceno));
  assert(bc_op(trace_startins_acq(T)) == loopop);
  ir = trace_ir_acq(T);
  for (r = REF_FIRST; r < trace_nins_acq(T); r++)
    if (ir[r].o == IR_CALLS &&
	ir[r].op2 == IRCALL_lj_tab_storeraw_rooted) {
      IRRef carg = ir[r].op1;
      uint32_t n = 0;
      while (carg >= REF_FIRST && carg < r && ir[carg].o == IR_CARG) {
	n++;
	carg = ir[carg].op1;
      }
      assert(n == cargc);
      calls++;
    }
  assert(calls > 0);
  return T;
}

static void assert_call_results(lua_State *L, const char *name, uint32_t wantc)
{
  GCproto *pt = get_proto(L, name);
  const BCIns *pc = proto_bc(pt), *end = pc + pt->sizebc;
  uint32_t calls = 0;
  for (; pc < end; pc++)
    if (bc_op(*pc) == BC_CALL) {
      assert(bc_b(*pc) == wantc);  /* nresults + 1 */
      assert(bc_c(*pc) == 4);      /* function plus table, key, value */
      calls++;
    }
  assert(calls == 1);
}

static uint32_t prime_anchor_boundary(lua_State *L, uint32_t *savedp)
{
  TGState *tg = L2TG(L);
  TValue nilv;
  uint32_t top = lj_tg_root_anchor_top_acq(tg), idx;
  *savedp = top;
  setnilV(&nilv);
  while ((top % TG_ROOT_ANCHOR_SLOTS) != TG_ROOT_ANCHOR_SLOTS - 1u) {
    assert(lj_tg_root_anchor_reserve_nothrow(L, tg));
    assert(lj_tg_root_anchor_push(L, tg, &nilv, &idx) != NULL);
    top = lj_tg_root_anchor_top_acq(tg);
  }
  return top;
}

static void restore_anchor_boundary(lua_State *L, uint32_t saved)
{
  TGState *tg = L2TG(L);
  lj_tg_root_anchor_rollback(tg, saved);
  assert(lj_tg_root_anchor_top_acq(tg) == saved);
}

static int exit_uses_post_store_numeric_snapshot(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  IRRef call = 0, r, n = trace_nins_acq(T);
  SnapNo s;
  for (r = REF_FIRST; r < n; r++)
    if (ir[r].o == IR_CALLS && ir[r].op2 == IRCALL_lj_tab_storeraw_rooted) {
      call = r;
      break;
    }
  if (!call)
    return 0;
  for (r = call + 2; r < n; r++)
    if (ir[r].o == IR_VLOAD && irt_type(ir[r].t) == IRT_NUM)
      for (s = 0; s < trace_nsnap_acq(T); s++)
	if (snap_ref_acq(&snap[s]) == r && r > call + 1 &&
	    lj_trace_test_last_exitno() == s)
	  return 1;
  return 0;
}

static void assert_clean(lua_State *L, uint32_t anchors, uint32_t readers)
{
  TGState *tg = L2TG(L);
  assert(lj_tg_root_anchor_top_acq(tg) == anchors);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  assert(lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

static void test_rawset_exits(const char *script)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  uint32_t anchors, readers, cas_before, total_before, alt_before;
  GCtrace *zero, *one, *after;
  int status;
  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, alternate_rawset);
  lua_setglobal(L, "rawset_alt");
  lua_pushcfunction(L, throwing_newindex);
  lua_setglobal(L, "rawset_throwing_newindex");
  status = luaL_dofile(L, script);
  if (status != LUA_OK) {
    fprintf(stderr, "rawset-exit fixture failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  tg = L2TG(L);
  anchors = lj_tg_root_anchor_top_acq(tg);
  readers = gc2_smr_readers_acq(G(L));
  lj_tab_test_set_store_post_cas_hook(count_store_cas);

  /* Warm both result forms and the post-store read. The hook distinguishes
  ** native effects from the same keyed CAS used by interpreter rawset. */
  for (uint32_t i = 0; i < 20; i++) {
    assert(call_kernel(L, "__arm64_rawset_zero", 0,
		       1000 + (lua_Integer)i * 64, 42.5, 32, 0, 0) == LUA_OK);
    assert(call_kernel(L, "__arm64_rawset_one_for", 0,
		       5000 + (lua_Integer)i * 64, 17.25, 32, 0, 0) == LUA_OK);
    assert(call_kernel(L, "__arm64_rawset_then_read", 0,
		       8000 + (lua_Integer)i * 64, 21.5, 32, 0, 0) == LUA_OK);
  }
  assert_call_results(L, "__arm64_rawset_zero", 1);
  assert_call_results(L, "__arm64_rawset_one_for", 2);
  zero = assert_trace(L, "__arm64_rawset_zero", BC_LOOP, 2);
  one = assert_trace(L, "__arm64_rawset_one_for", BC_FORL, 2);
  assert(la_load32_acq(&rooted_cas_calls) > 0);

  /* Root-anchor reserve failure happens after native entry and before any
  ** helper effect. The status guard must resume at CALL; interpreter rawset
  ** inserts once despite the throwing __newindex metatable. */
  cas_before = la_load32_acq(&rooted_cas_calls);
  total_before = la_load32_acq(&target_cas_calls);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_kernel(L, "__arm64_rawset_zero", 0,
		     900001, 73.5, 1, 0, 1) == LUA_OK);
  assert(la_load32_acq(&rooted_cas_calls) == cas_before);
  assert(la_load32_acq(&target_cas_calls) == total_before + 1u);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == zero->traceno &&
	 lj_trace_test_last_exit_parent() == zero->traceno);
  assert(la_load32_acq(&newindex_calls) == 0);
  assert_clean(L, anchors, readers);

  /* A CCI_T unwind during root handoff restores the pre-CALL Lua frame and
  ** releases its anchors without publishing or replaying the store. */
  total_before = la_load32_acq(&target_cas_calls);
  lj_tab_test_set_rooted_gc_store_hook(throw_before_store);
  assert(call_kernel(L, "__arm64_rawset_zero", 0,
                     915001, 21.5, 1, 2, 0) == LUA_ERRRUN);
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  assert(helper_throws == 1);
  assert(la_load32_acq(&target_cas_calls) == total_before);
  assert_clean(L, anchors, readers);

  /* Change only the call target after warming the one-result FORL. The exact
  ** rawset-function guard must exit before the call, then the alternate C
  ** function performs the single interpreter raw write and returns the table. */
  cas_before = la_load32_acq(&rooted_cas_calls);
  total_before = la_load32_acq(&target_cas_calls);
  alt_before = la_load32_acq(&alternate_calls);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_kernel(L, "__arm64_rawset_one_for", 1,
		     910001, 19.5, 2, 0, 0) == LUA_OK);
  /* The first FORL iteration runs in the interpreter; the second reaches
  ** the native function-identity guard and resumes the alternate call. */
  assert(la_load32_acq(&alternate_calls) == alt_before + 2u);
  assert(la_load32_acq(&rooted_cas_calls) == cas_before);
  assert(la_load32_acq(&target_cas_calls) == total_before + 2u);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == one->traceno &&
	 lj_trace_test_last_exit_parent() == one->traceno);
  assert_clean(L, anchors, readers);

  /* The CAS succeeds once, then the test hook changes the stored value to a
  ** boolean. The following guarded arithmetic exits after CALL completion.
  ** Error plus retained boolean proves the interpreter did not replay rawset. */
  after = assert_trace(L, "__arm64_rawset_then_read", BC_LOOP, 2);
  cas_before = la_load32_acq(&rooted_cas_calls);
  total_before = la_load32_acq(&target_cas_calls);
  la_store32_rel(&mutated_after_cas, 0);
  la_store32_rel(&mutate_after_cas, 1);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_kernel(L, "__arm64_rawset_then_read", 0,
		     920001, 23.5, 1, 1, 0) == LUA_ERRRUN);
  assert(la_load32_acq(&mutate_after_cas) == 0);
  assert(la_load32_acq(&mutated_after_cas) == 1u);
  assert(la_load32_acq(&rooted_cas_calls) == cas_before + 1u);
  assert(la_load32_acq(&target_cas_calls) == total_before + 1u);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == after->traceno &&
	 lj_trace_test_last_exit_parent() == after->traceno);
  assert(exit_uses_post_store_numeric_snapshot(after));
  assert_clean(L, anchors, readers);
  assert(la_load32_acq(&newindex_calls) == 0);

  lj_tab_test_set_store_post_cas_hook(NULL);
  run_chunk(L, "__arm64_rawset_release()");
  lua_close(L);
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  test_rawset_exits(argv[1]);
  puts("t-arm64-jit-rawset-exits OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-rawset-exits SKIP");
  return 0;
}

#endif
