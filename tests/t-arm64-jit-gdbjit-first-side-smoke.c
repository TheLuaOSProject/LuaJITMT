/*
** Production-configuration smoke for GDBJIT on the exact ARM64 first side.
**
** This fixture and its LuaJIT archive are built without trace or GDBJIT test
** helpers. It proves that an ordinary root and its admitted first child both
** receive distinct debugger objects without changing native execution.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_gdbjit.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#if !defined(__APPLE__) || \
    (!defined(__aarch64__) && !defined(__arm64__)) || \
    !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    !defined(LUAJIT_MT_ARM64_BOOTSTRAP) || \
    !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !defined(LUAJIT_USE_GDBJIT) || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
#error "fixture requires production exact-first-side ARM64 GDBJIT"
#endif

#if defined(LJ_TRACE_TEST_HELPERS) || defined(LJ_GDBJIT_TEST_HELPERS) || \
    defined(LJ_ARM64_FIRST_SIDE_PUBLISH_TEST) || \
    defined(LJ_ARM64_SIDE_ASM_TEST)
#error "production GDBJIT first-side smoke must not use test helpers"
#endif

static GDBJITentry *entry_next_acq(const GDBJITentry *entry)
{
  return (GDBJITentry *)la_loadptr_acq(
    (void *const *)&entry->next_entry);
}

static GDBJITentry *entry_prev_acq(const GDBJITentry *entry)
{
  return (GDBJITentry *)la_loadptr_acq(
    (void *const *)&entry->prev_entry);
}

static GDBJITentry *descriptor_first_acq(void)
{
  return (GDBJITentry *)la_loadptr_acq(
    (void *const *)&__jit_debug_descriptor.first_entry);
}

static GDBJITentry *descriptor_relevant_acq(void)
{
  return (GDBJITentry *)la_loadptr_acq(
    (void *const *)&__jit_debug_descriptor.relevant_entry);
}

static uintptr_t pointer_bits(const void *pointer)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(pointer), "pointer width mismatch");
  memcpy(&bits, &pointer, sizeof(bits));
  return bits;
}

#if LJ_ABI_PAUTH
static uintptr_t function_bits(ASMFunction function)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(function), "function width mismatch");
  memcpy(&bits, &function, sizeof(bits));
  return bits;
}
#endif

static void assert_quiescent(lua_State *L, jit_State *J)
{
  assert(J->curfinal == NULL);
  assert(J->gdbjit_pending_abort == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(G(L)) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(gc2_smr_readers_acq(G(L)) == 0);
}

static const char fixture_lua[] =
  "function __gdbjit_first_side_smoke(n, bias)\n"
  "  local i=0\n"
  "  while i<n do\n"
  "    i=i+1\n"
  "    if bias~=0 then i=i+1 end\n"
  "  end\n"
  "  return i\n"
  "end\n";

static lua_Integer call_pair(lua_State *L, lua_Integer n, lua_Integer bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, "__gdbjit_first_side_smoke");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production GDBJIT call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static GCproto *fixture_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__gdbjit_first_side_smoke");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  GCproto *pt;
  GCtrace *root = NULL, *child = NULL;
  SnapShot *rootsnap;
  MCode **root_exittab;
  MCode *root_fallback, *child_mcode;
  void *root_fallback_raw, *child_raw;
  TraceNo rootno = 0, childno = 0;
  GDBJITentry *root_entry, *child_entry;
  unsigned attempt;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')") == LUA_OK);
  assert(luaL_loadbuffer(L, fixture_lua, sizeof(fixture_lua)-1u,
	"@gdbjit-first-side-smoke.lua") == LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
  g = G(L);
  J = G2J(g);
  pt = fixture_proto(L);

  for (attempt = 0; attempt < 64; attempt++) {
    assert(call_pair(L, 3, 0) == 3);
    rootno = proto_trace_acq(pt);
    if (rootno != 0) {
      root = traceref_safe(J, rootno);
      if (trace_runnable_acq(root, rootno))
	break;
    }
  }
  assert(root != NULL && rootno != 0);
  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == rootno);
  assert(trace_linktype_acq(root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  rootsnap = trace_snap_acq(root);
  root_exittab = trace_exittab_acq(root);
  assert(rootsnap != NULL && trace_nsnap_acq(root) > 2);
  assert(snap_count_acq(&rootsnap[2]) < SNAPCOUNT_DONE);
  assert(root_exittab != NULL);
  root_fallback_raw = la_loadptr_acq((void *const *)&root_exittab[2]);
  root_fallback = trace_exittarget_arm64_acq(root, 2);
  assert(root_fallback != NULL);
  root_entry = (GDBJITentry *)trace_gdbjit_entry_acq(root);
  assert(root_entry != NULL);
  assert(la_load32_acq(&__jit_debug_descriptor.version) == 1);
  assert(la_load32_acq(&__jit_debug_descriptor.action_flag) == GDBJIT_REGISTER);
  assert(descriptor_first_acq() == root_entry);
  assert(descriptor_relevant_acq() == root_entry);
  assert(entry_next_acq(root_entry) == NULL);
  assert(entry_prev_acq(root_entry) == NULL);
  assert(la_loadptr_acq((void *const *)&root_entry->symfile_addr) != NULL);
  assert(la_load64_acq(&root_entry->symfile_size) != 0);

  for (attempt = 0; attempt < 8; attempt++) {
    assert(call_pair(L, 3, 1) == 4);
    childno = trace_nextside_acq(root);
    if (childno != 0) {
      child = traceref_safe(J, childno);
      if (trace_runnable_acq(child, childno))
	break;
    }
  }
  assert(child != NULL && childno != 0 && childno != rootno);
  assert(trace_root_acq(child) == rootno);
  assert(trace_link_acq(child) == rootno);
  assert(trace_linktype_acq(child) == LJ_TRLINK_ROOT);
  assert(trace_startpt_acq(child) == pt);
  assert(trace_nchild_acq(root) == 1);
  assert(trace_nextside_acq(root) == childno);
  assert(snap_count_acq(&rootsnap[2]) == SNAPCOUNT_DONE);
  assert((GDBJITentry *)trace_gdbjit_entry_acq(root) == root_entry);
  child_entry = (GDBJITentry *)trace_gdbjit_entry_acq(child);
  assert(child_entry != NULL && child_entry != root_entry);
  child_mcode = trace_mcode_acq(child);
  assert(child_mcode != NULL);
  child_raw = la_loadptr_acq((void *const *)&root_exittab[2]);
  assert(pointer_bits(child_raw) ==
	 pointer_bits(trace_exittarget_arm64_encode(g, child_mcode)));
  assert(trace_exittarget_arm64_acq(root, 2) == child_mcode);
#if LJ_ABI_BRANCH_TRACK
  assert(child_mcode[0] == A64I_LE(A64I_BTI_J));
#endif
#if LJ_ABI_PAUTH
  {
    ASMFunction actual = trace_mcauth_acq(child);
    ASMFunction expected = lj_ptr_sign(
	ptrauth_nop_cast(ASMFunction, child_mcode), child);
    assert(function_bits(actual) == function_bits(expected));
    assert(ptrauth_nop_cast(MCode *, lj_ptr_strip(actual)) == child_mcode);
  }
#endif
  assert(la_load32_acq(&__jit_debug_descriptor.action_flag) == GDBJIT_REGISTER);
  assert(descriptor_first_acq() == child_entry);
  assert(descriptor_relevant_acq() == child_entry);
  assert(entry_prev_acq(child_entry) == NULL);
  assert(entry_next_acq(child_entry) == root_entry);
  assert(entry_prev_acq(root_entry) == child_entry);
  assert(entry_next_acq(root_entry) == NULL);
  assert(la_loadptr_acq((void *const *)&child_entry->symfile_addr) != NULL);
  assert(la_load64_acq(&child_entry->symfile_size) != 0);
  assert_quiescent(L, J);

  for (attempt = 0; attempt < 4; attempt++)
    assert(call_pair(L, 3, 1) == 4);
  assert(trace_runnable_acq(root, rootno));
  assert(trace_runnable_acq(child, childno));
  assert(trace_nextside_acq(root) == childno);
  assert((GDBJITentry *)trace_gdbjit_entry_acq(root) == root_entry);
  assert((GDBJITentry *)trace_gdbjit_entry_acq(child) == child_entry);
  assert(pointer_bits(la_loadptr_acq((void *const *)&root_exittab[2])) ==
	 pointer_bits(trace_exittarget_arm64_encode(g, child_mcode)));
  assert_quiescent(L, J);

  assert(lj_trace_flushscope(J, childno) == 1u);
  assert(trace_gdbjit_entry_acq(child) == NULL);
  assert((GDBJITentry *)trace_gdbjit_entry_acq(root) == root_entry);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(pointer_bits(la_loadptr_acq((void *const *)&root_exittab[2])) ==
	 pointer_bits(root_fallback_raw));
  assert(trace_exittarget_arm64_acq(root, 2) == root_fallback);
  assert(la_load32_acq(&__jit_debug_descriptor.action_flag) ==
	 GDBJIT_UNREGISTER);
  assert(descriptor_first_acq() == root_entry);
  assert(entry_prev_acq(root_entry) == NULL);
  assert(entry_next_acq(root_entry) == NULL);
  assert_quiescent(L, J);

  assert(lj_trace_flushall_gc(L) == 0);
  assert(trace_gdbjit_entry_acq(root) == NULL);
  assert(proto_trace_acq(pt) == 0);
  assert(la_load32_acq(&__jit_debug_descriptor.action_flag) ==
	 GDBJIT_UNREGISTER);
  assert(descriptor_first_acq() == NULL);
  assert_quiescent(L, J);

  lua_close(L);
  puts("t-arm64-jit-gdbjit-first-side-smoke OK");
  return 0;
}
