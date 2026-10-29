/*
** Ordinary-production proof for the exact first ARM64 side trace.
**
** This fixture is intentionally built without either ARM64 side test seam.
** The no-helper build is an embedded-Lua smoke test.  The helper build adds
** white-box checks for four independently recorded prototype/root/child pairs,
** native entry, authenticated exit-table representation, and retirement.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED != 0 || \
    LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1 || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED
#error "production first-side probe requires admitted roots/first sides"
#endif

#if defined(LJ_ARM64_FIRST_SIDE_PUBLISH_TEST) || \
    defined(LJ_ARM64_SIDE_ASM_TEST)
#error "production first-side probe must not use an ARM64 side test seam"
#endif

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production first-side Lua failure: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer call_named(lua_State *L, const char *name,
	lua_Integer n, lua_Number bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushnumber(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production first-side call %s failed: %s\n",
	    name, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static lua_Integer call_forl_named(lua_State *L, const char *name,
	lua_Integer start, lua_Integer stop, lua_Integer mode)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, start);
  lua_pushinteger(L, stop);
  lua_pushinteger(L, mode);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production FORL call %s failed: %s\n",
	    name, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static int side_opaque_index(lua_State *L)
{
  (void)L;
  lua_pushinteger(L, 3);
  return 1;
}

static void define_probes(lua_State *L)
{
  lua_pushcfunction(L, side_opaque_index);
  lua_setglobal(L, "__arm64_first_side_opaque_index");
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=24'); "
    "local function decoy(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=i+1 "
        "if bias~=0 then i=i+1 end "
      "end "
      "return i "
    "end "
    "local function first(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=i+1 "
        "if bias~=0 then i=i+1 end "
      "end "
      "return i "
    "end "
    "local function second(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=(i~=0 and i or i)+1 "
      "end "
      "return i "
    "end "
    "local function third(n, bias) "
      "local i=0 "
      "while i<n do "
        "if i==0 then i=i+1 end "
        "i=i+1 "
      "end "
      "return i "
    "end "
    "local function fourth(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=(i~=0 and i or i)+2 "
      "end "
      "return i "
    "end "
    "local function loopchild(n, step) "
      "local i,sum=1,0 "
      "while i<=n do sum=sum+step; i=i+1 end "
      "return sum "
    "end "
    "local function forlchild(start, n, mode) "
      "local sum=0 "
      "for i=start,n do "
        "if mode==1 then sum=sum+i "
        "elseif mode==2 then sum=sum-i "
        "else sum=sum+i*3 end "
      "end "
      "return sum "
    "end "
    "local function forlchildneg(start, n, mode) "
      "local sum=0 "
      "for i=start,n,-1 do "
        "if mode==1 then sum=sum+i "
        "elseif mode==2 then sum=sum-i "
        "else sum=sum+i*3 end "
      "end "
      "return sum "
    "end "
    "local opaque=setmetatable({}, {__index=__arm64_first_side_opaque_index}) "
    "local function unsupported(n, bias) "
      "local i=0 "
      "while i<n do "
      "if bias~=0 then i=i+opaque.value else i=i+3 end "
      "end "
      "return i "
    "end "
    "__arm64_first_side_production_decoy=decoy; "
    "__arm64_first_side_production_first=first; "
    "__arm64_first_side_production_second=second; "
    "__arm64_first_side_production_third=third; "
    "__arm64_first_side_production_fourth=fourth; "
    "__arm64_first_side_production_loopchild=loopchild; "
    "__arm64_first_side_production_forlchild=forlchild; "
    "__arm64_first_side_production_forlchildneg=forlchildneg; "
    "__arm64_first_side_production_unsupported=unsupported");
}

#ifdef LJ_TRACE_TEST_HELPERS
static void start_live_peer(lua_State *L)
{
  run_lua(L,
    "local threading=require('threading'); "
    "local ready,stop=threading.channel(1),threading.channel(1); "
    "local peer=assert(threading.spawn(function(r,s) "
      "assert(r:send(true,5)); "
      "assert(select(2,s:recv(120))); return true end,ready,stop)); "
    "assert(select(2,ready:recv(5))); "
    "__arm64_first_side_peer_stop=function() "
      "assert(stop:send(true,5)); "
      "local joined,ok=peer:join(5); assert(joined and ok) end");
}

static void stop_live_peer(lua_State *L)
{
  run_lua(L, "__arm64_first_side_peer_stop(); "
    "__arm64_first_side_peer_stop=nil");
}
#endif

enum { PRODUCTION_ROOT_ATTEMPTS = 64 };

#ifndef LJ_TRACE_TEST_HELPERS

static int smoke_main(void)
{
  lua_State *L = luaL_newstate();
  unsigned attempt;
  assert(L != NULL);
  luaL_openlibs(L);
  define_probes(L);

  /* Run the two same-bytecode exit-6 variants before unrelated hot-count
  ** slots can collide. The +2 root is recorded with n=3 and its admitted
  ** root-linked child with n=5; repeated +3 calls retain only the root. */
  assert(call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  for (attempt = 0; attempt < PRODUCTION_ROOT_ATTEMPTS; attempt++)
    assert(call_named(L, "__arm64_first_side_production_unsupported",
	4, 0) == 6);

  assert(call_named(L, "__arm64_first_side_production_decoy", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 1) == 4);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 1) == 4);
  assert(call_named(L, "__arm64_first_side_production_second", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_second", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_second", 2, 0) == 2);
  assert(call_named(L, "__arm64_first_side_production_third", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_loopchild", 64, 1) == 64);
  assert(call_named(L, "__arm64_first_side_production_loopchild", 64, 1.25) == 80);
  assert(call_named(L, "__arm64_first_side_production_loopchild", 64, 1.25) == 80);
  assert(call_forl_named(L, "__arm64_first_side_production_forlchild", 1,
			 64, 1) == 2080);
  assert(call_forl_named(L, "__arm64_first_side_production_forlchild", 1,
			 64, 2) == -2080);
  assert(call_forl_named(L, "__arm64_first_side_production_forlchild", 1,
			 64, 2) == -2080);
  assert(call_named(L, "__arm64_first_side_production_third", 4, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_third", 4, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_third", 3, 0) == 3);
  run_lua(L,
    "local util=require('jit.util'); local live, roots, sides=0,0,0; "
    "for tr=1,32 do local i=util.traceinfo(tr); if i then "
      "live=live+1; "
      "if i.linktype=='loop' then roots=roots+1 "
      "elseif i.linktype=='root' then sides=sides+1 end "
    "end end; "
    "if live~=14 then for tr=1,32 do local i=util.traceinfo(tr); "
      "if i then io.stderr:write('trace ',tr,' link=',i.link, "
        "' type=',i.linktype,' exit=',i.nexit,'\\n') end end end; "
    "assert(live==14, 'expected fourteen production traces, got '..live); "
    "assert(roots==9, 'expected nine loop traces, got '..roots); "
    "assert(sides==5, 'expected five root-linked first sides, got '..sides)");

  lua_close(L);
  puts("t-arm64-jit-first-side-production smoke OK");
  return 0;
}

#else

#include "lj_arena.h"
#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_safepoint.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

static void set_hotexit(lua_State *L, unsigned threshold)
{
  char source[64];
  snprintf(source, sizeof(source),
	   "require('jit.opt').start('hotexit=%u')", threshold);
  run_lua(L, source);
}

enum {
  PRODUCTION_PAIR_COUNT = 7
};

typedef struct ProductionPair {
  const char *name;
  lua_Integer root_n;
  lua_Number root_bias;
  lua_Integer root_result;
  lua_Integer side_n;
  lua_Number side_bias;
  lua_Integer side_result;
  lua_Integer native_n;
  lua_Number native_bias;
  lua_Integer native_result;
  ExitNo exitno;
  int require_loop_child;
  int require_forl_root;
  const char *oracle_name;
  lua_Integer root_start;
  lua_Integer side_start;
  lua_Integer native_start;
  GCproto *pt;
  TraceNo rootno;
  TraceNo childno;
  GCtrace *root;
  GCtrace *child;
  const BCIns *continuation;
  SnapShot *root_snap;
  MCode **root_exittab;
  MCode *root_fallback;
  MCode *child_mcode;
} ProductionPair;

static lua_Integer call_pair(lua_State *L, const ProductionPair *pair,
	unsigned phase)
{
  lua_Integer n = phase == 0 ? pair->root_n :
		  phase == 1 ? pair->side_n : pair->native_n;
  lua_Number mode = phase == 0 ? pair->root_bias :
		    phase == 1 ? pair->side_bias : pair->native_bias;
  if (pair->require_forl_root) {
	lua_Integer start = phase == 0 ? pair->root_start :
			    phase == 1 ? pair->side_start : pair->native_start;
	return call_forl_named(L, pair->name, start, n, (lua_Integer)mode);
  }
  return call_named(L, pair->name, n, mode);
}

static GCproto *named_proto(lua_State *L, const char *name)
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

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static uintptr_t pointer_bits(void *target)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(target), "pointer width mismatch");
  memcpy(&bits, &target, sizeof(bits));
  return bits;
}

#if LJ_ABI_PAUTH
static uintptr_t function_bits(ASMFunction target)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(target), "function width mismatch");
  memcpy(&bits, &target, sizeof(bits));
  return bits;
}
#endif

static GCtrace *find_root(jit_State *J, GCproto *pt, TraceNo *tracenop)
{
  TraceNo traceno;
  GCtrace *found = NULL;
  TraceNo foundno = 0;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (trace_runnable_acq(T, traceno) && trace_startpt_acq(T) == pt &&
	trace_root_acq(T) == 0 &&
	trace_linktype_acq(T) == LJ_TRLINK_LOOP) {
      assert(found == NULL);
      found = T;
      foundno = traceno;
    }
  }
  if (tracenop != NULL)
    *tracenop = foundno;
  return found;
}

static uint32_t live_trace_count(jit_State *J)
{
  TraceNo traceno;
  uint32_t count = 0;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    if (trace_runnable_acq(traceref_safe(J, traceno), traceno))
      count++;
  return count;
}

static const BCIns *selected_continuation(const GCtrace *T, ExitNo exitno)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  SnapShot *selected;
  assert(snap != NULL && map != NULL && exitno < trace_nsnap_acq(T));
  selected = &snap[exitno];
  return snap_pc_acq(&map[snap_mapofs_acq(selected) +
			  snap_nent_acq(selected)]);
}

static int side_parent_cert_zero(const LJTraceArm64SideParentCert *cert)
{
  return cert->tracev == NULL && cert->body == NULL && cert->mcode == NULL &&
	 cert->continuation == NULL && cert->continuationins == 0 &&
	 cert->parent == 0 && cert->exitno == 0 && cert->child == 0;
}

static void expect_edge(global_State *g, const ProductionPair *pair,
	MCode *target)
{
  MCode **exittab = trace_exittab_acq(pair->root);
  void *raw = la_loadptr_acq(
	(void *const *)&exittab[pair->exitno]);
  void *encoded = trace_exittarget_arm64_encode(g, target);
  assert(pointer_bits(raw) == pointer_bits(encoded));
  assert(trace_exittarget_arm64_acq(pair->root, pair->exitno) == target);
}

static void record_root(lua_State *L, jit_State *J, ProductionPair *pair)
{
  unsigned attempt;
  /* Keep initial root execution from recording an unrelated side before the
  ** fixture has captured its root snapshot and can arm the intended exit. */
  set_hotexit(L, 100000);
  pair->pt = named_proto(L, pair->name);
  for (attempt = 0; attempt < PRODUCTION_ROOT_ATTEMPTS; attempt++) {
    assert(call_pair(L, pair, 0) == pair->root_result);
    pair->root = find_root(J, pair->pt, &pair->rootno);
    if (pair->root != NULL)
      break;
  }
  assert(pair->root != NULL && pair->rootno != 0);
}

static void expect_root_shape(jit_State *J, global_State *g,
	ProductionPair *pair)
{
  const BCIns *startpc;
  BCIns live;
  BCOp rootop = pair->require_forl_root ? BC_FORL : BC_LOOP;
  assert(pair->pt != NULL && pair->pt->sizebc != 0);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_traceno_acq(pair->root) == pair->rootno);
  assert(trace_root_acq(pair->root) == 0);
  assert(trace_link_acq(pair->root) == pair->rootno);
  assert(trace_linktype_acq(pair->root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  assert(trace_startpt_acq(pair->root) == pair->pt);
  assert(trace_topslot_acq(pair->root) <= pair->pt->framesize);
  assert(trace_spadjust_acq(pair->root) == 0);
  assert(trace_mcode_acq(pair->root) != NULL);
  assert((trace_arm64_admission_acq(pair->root) &
	  (pair->require_forl_root ? TRACE_ARM64_INT_FORL_ADMITTED :
	   TRACE_ARM64_INT_LOOP_ADMITTED)) != 0);
  assert((trace_arm64_admission_acq(pair->root) &
	  TRACE_ARM64_INT_SIDE_ADMITTED) == 0);

  startpc = trace_startpc_acq(pair->root);
  assert(startpc != NULL && startpc >= proto_bc(pair->pt) &&
	 startpc < proto_bc(pair->pt)+pair->pt->sizebc);
  assert(bc_op(trace_startins_acq(pair->root)) == rootop);
  live = loadbc(startpc);
  assert(bc_op(live) == (pair->require_forl_root ? BC_JFORL : BC_JLOOP) &&
	 bc_d(live) == pair->rootno);
  if (pair->require_forl_root) {
	BCPos pos = proto_bcpos(pair->pt, startpc);
	BCPos body = pos+1u+bc_j(trace_startins_acq(pair->root));
	BCIns fori;
	assert(body > 0 && body <= pos);
	fori = loadbc(&proto_bc(pair->pt)[body-1u]);
	assert(bc_op(fori) == BC_FORI &&
	       bc_a(fori) == bc_a(trace_startins_acq(pair->root)) &&
	       bc_j(fori) > 0 && body+bc_j(fori) == pos+1u);
  }
  assert(proto_trace_acq(pair->pt) == pair->rootno);

  assert(pair->exitno < trace_nsnap_acq(pair->root));
  assert(trace_exittab_nslots_acq(pair->root) ==
	 trace_nsnap_acq(pair->root));
  pair->root_snap = trace_snap_acq(pair->root);
  pair->root_exittab = trace_exittab_acq(pair->root);
  pair->root_fallback = trace_exittarget_arm64_acq(pair->root,
						 pair->exitno);
  assert(pair->root_exittab != NULL && pair->root_fallback != NULL);
  pair->continuation = selected_continuation(pair->root, pair->exitno);
  assert(pair->continuation >= proto_bc(pair->pt) &&
	 pair->continuation < proto_bc(pair->pt)+pair->pt->sizebc);
  assert(snap_count_acq(&pair->root_snap[pair->exitno]) != SNAPCOUNT_DONE);
  expect_edge(g, pair, pair->root_fallback);
  assert(traceref_safe(J, pair->rootno) == pair->root);
}

static void record_child(lua_State *L, jit_State *J, ProductionPair *pair)
{
  unsigned attempt;
#ifdef LJ_TRACE_TEST_HELPERS
  if (pair->require_forl_root)
    run_lua(L,
	"__forl_exits={}; function __forl_exit(no, ex) "
	"__forl_exits[# __forl_exits+1]={no,ex} end; "
	"jit.off(__forl_exit,true); jit.attach(__forl_exit,'texit')");
#endif
  for (attempt = 0; attempt < 128; attempt++) {
    TraceNo childno;
    assert(call_pair(L, pair, 1) == pair->side_result);
    childno = trace_nextside_acq(pair->root);
    if (childno != 0) {
      GCtrace *child = traceref_safe(J, childno);
      if (trace_runnable_acq(child, childno)) {
	pair->childno = childno;
	pair->child = child;
	break;
      }
    }
  }
  if (pair->child == NULL || pair->childno == 0) {
#ifdef LJ_TRACE_TEST_HELPERS
    if (pair->require_forl_root)
      run_lua(L,
	"jit.attach(__forl_exit); for i=1,#__forl_exits do "
	"io.stderr:write('FORL_EXIT trace=',__forl_exits[i][1], "
	"' exit=',__forl_exits[i][2],'\\n') end");
#endif
    fprintf(stderr, "first-side child missing name=%s root=%u op=%u exit=%u "
	"next=%u aborts=%u last=%u\n", pair->name,
	(unsigned)pair->rootno, (unsigned)bc_op(trace_startins_acq(pair->root)),
	(unsigned)pair->exitno, (unsigned)trace_nextside_acq(pair->root),
	(unsigned)lj_trace_test_abort_count(),
	(unsigned)lj_trace_test_last_abort_error());
    assert(pair->child != NULL && pair->childno != 0);
  }
}

static void expect_post_token_request_cleanup(lua_State *L, jit_State *J,
	global_State *g, TGState *tg, ProductionPair *pair)
{
  uint64_t epoch = gc2_hs_epoch_acq(g);
  uint32_t initial = (uint32_t)snap_count_acq(
	&pair->root_snap[pair->exitno]);
  uint32_t before;
  unsigned attempt;
  int status;

  assert(initial != SNAPCOUNT_DONE);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_STOPREQ);
  /* Re-enter the root until its hot-exit counter starts the selected side.
  ** The generic recorder can defer that trigger beyond a single call. */
  status = 0;
  for (attempt = 0; attempt < 128; attempt++) {
    lua_getglobal(L, pair->name);
    assert(lua_isfunction(L, -1));
    if (pair->require_forl_root) {
      lua_pushinteger(L, pair->side_start);
      lua_pushinteger(L, pair->side_n);
      lua_pushinteger(L, (lua_Integer)pair->side_bias);
      status = lua_pcall(L, 3, 1, 0);
    } else {
      lua_pushinteger(L, pair->side_n);
      lua_pushinteger(L, pair->side_bias);
      status = lua_pcall(L, 2, 1, 0);
    }
    if (lj_trace_test_admission_hits() != 0)
      break;
    if (status != 0)
      break;
    lua_pop(L, 1);
  }
  assert(status == 0 ||
	 (status == LUA_ERRRUN && lua_isstring(L, -1)));
  if (status == 0) {
    lua_pop(L, 1);
    assert(lj_safepoint_owner_poll_pending(L));
    (void)lj_safepoint_poll(L);
  }
  before = lj_trace_test_admission_side_snapshot_before();
  assert(before != SNAPCOUNT_DONE);
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_side_parent() == pair->rootno);
  pair->exitno = lj_trace_test_admission_side_exitno();
  assert(pair->exitno < trace_nsnap_acq(pair->root));
  pair->root_snap = trace_snap_acq(pair->root);
  pair->root_exittab = trace_exittab_acq(pair->root);
  pair->root_fallback = trace_exittarget_arm64_acq(pair->root,
						 pair->exitno);
  pair->continuation = selected_continuation(pair->root, pair->exitno);
  assert(lj_trace_test_admission_side_snapshot_before() == before);
  assert(lj_trace_test_admission_side_gate_blocks() == 0);
  assert(lj_trace_test_admission_side_clean_releases() == 1);
  assert((uint32_t)snap_count_acq(&pair->root_snap[pair->exitno]) ==
	 before);
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  expect_edge(g, pair, pair->root_fallback);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(side_parent_cert_zero(&J->arm64_side_parent));
}

static void expect_child_shape(jit_State *J, global_State *g,
	ProductionPair *pair)
{
  IRIns *ir = trace_ir_acq(pair->child);
  SnapShot *snap = trace_snap_acq(pair->child);
  SnapEntry *snapmap = trace_snapmap_acq(pair->child);
  MCode **exittab = trace_exittab_acq(pair->child);
  MCode *fallback = exitstub_trace_fallback_addr_(
	trace_exitstub_acq(pair->child));
  SnapNo snapno;
  GCArena *arena;
  IRIns base;

  assert(ir != NULL && snap != NULL && snapmap != NULL && exittab != NULL);
  base = ir_load_acq(&ir[REF_BASE]);
  assert(base.o == IR_BASE && base.op1 == pair->rootno &&
	 base.op2 < trace_nsnap_acq(pair->root));
  /* Admission can retry after the interrupted post-token attempt and record
  ** a different hot exit. Bind lifecycle checks to the child's own IR_BASE. */
  pair->exitno = (ExitNo)base.op2;
  pair->root_snap = trace_snap_acq(pair->root);
  pair->root_exittab = trace_exittab_acq(pair->root);
  pair->root_fallback = exitstub_trace_fallback_addr_(
	trace_exitstub_acq(pair->root));
  pair->continuation = selected_continuation(pair->root, pair->exitno);

  assert(trace_runnable_acq(pair->child, pair->childno));
  assert(trace_traceno_acq(pair->child) == pair->childno);
  assert(trace_root_acq(pair->child) == pair->rootno);
  assert(trace_linktype_acq(pair->child) ==
	 (pair->require_loop_child ? LJ_TRLINK_LOOP : LJ_TRLINK_ROOT));
  if (trace_linktype_acq(pair->child) == LJ_TRLINK_ROOT) {
    assert(trace_link_acq(pair->child) == pair->rootno);
    assert(trace_mcloop_acq(pair->child) == 0);
  } else {
    assert(trace_linktype_acq(pair->child) == LJ_TRLINK_LOOP);
    assert(trace_link_acq(pair->child) == pair->childno);
    assert(trace_mcloop_acq(pair->child) != 0 &&
	   trace_mcloop_acq(pair->child) < trace_szmcode_acq(pair->child) &&
	   (trace_mcloop_acq(pair->child) & (sizeof(MCode)-1u)) == 0);
  }
  assert(trace_nextroot_acq(pair->child) == 0);
  assert(trace_nextside_acq(pair->child) == 0);
  assert(trace_nchild_acq(pair->child) == 0);
  assert(trace_startpt_acq(pair->child) == pair->pt);
  assert(trace_startpc_acq(pair->child) == pair->continuation);
  assert(trace_startins_acq(pair->child) == BCINS_AD(BC_JMP, 0, 0));
  assert(trace_topslot_acq(pair->child) == trace_topslot_acq(pair->root));
  assert(trace_spadjust_acq(pair->child) == 0);
  assert(trace_nsnap_acq(pair->child) != 0);
  assert(trace_exittab_nslots_acq(pair->child) ==
	 trace_nsnap_acq(pair->child)+1u);
  assert(trace_nk_acq(pair->child) < trace_nins_acq(pair->child));
  assert(trace_arm64_admission_acq(pair->child) ==
	 TRACE_ARM64_INT_SIDE_ADMITTED);
  assert(trace_native_pinword_acq(pair->child) == 0);
  assert(la_load64_acq(&pair->child->retire_epoch) == 0);

  for (snapno = 0; snapno < trace_nsnap_acq(pair->child); snapno++) {
    SnapShot *ss = &snap[snapno];
    MSize mapofs = snap_mapofs_acq(ss);
    MSize nent = snap_nent_acq(ss);
    const BCIns *pc;
    assert(mapofs <= trace_nsnapmap_acq(pair->child));
    assert(nent < trace_nsnapmap_acq(pair->child)-mapofs);
    pc = snap_pc_acq(&snapmap[mapofs+nent]);
    assert(pc >= proto_bc(pair->pt) &&
	   pc < proto_bc(pair->pt)+pair->pt->sizebc);
    assert(snap_topslot_acq(ss) == trace_topslot_acq(pair->child));
  }

  pair->child_mcode = trace_mcode_acq(pair->child);
  assert(pair->child_mcode != NULL &&
	 trace_szmcode_acq(pair->child) > sizeof(MCode));
  if (trace_exittarget_arm64_acq(pair->root, pair->exitno) != pair->child_mcode) {
    ExitNo exitno;
    fprintf(stderr,
	    "first-side edge mismatch root=%u child=%u exit=%u base=(%u,%u) "
	    "link=(%u,%u) admission=%u code=%p\n",
	    pair->rootno, pair->childno, pair->exitno,
	    (unsigned)base.op1, (unsigned)base.op2,
	    (unsigned)trace_link_acq(pair->child),
	    (unsigned)trace_linktype_acq(pair->child),
	    (unsigned)trace_arm64_admission_acq(pair->child),
	    (void *)pair->child_mcode);
	for (exitno = 0; exitno < trace_nsnap_acq(pair->root); exitno++)
	  fprintf(stderr, "  exit %u -> %p\n", (unsigned)exitno,
		  (void *)trace_exittarget_arm64_acq(pair->root, exitno));
	assert(0);
  }
  for (snapno = 0; snapno < trace_exittab_nslots_acq(pair->child); snapno++) {
    void *raw = la_loadptr_acq((void *const *)&exittab[snapno]);
    void *encoded = trace_exittarget_arm64_encode(g, fallback);
    assert(pointer_bits(raw) == pointer_bits(encoded));
    assert(trace_exittarget_arm64_acq(pair->child, (ExitNo)snapno) ==
	   fallback);
  }
#if LJ_ABI_BRANCH_TRACK
  assert(pair->child_mcode[0] == A64I_LE(A64I_BTI_J));
#endif
#if LJ_ABI_PAUTH
  {
    ASMFunction actual = trace_mcauth_acq(pair->child);
    ASMFunction expected = lj_ptr_sign(
	ptrauth_nop_cast(ASMFunction, pair->child_mcode), pair->child);
    assert(function_bits(actual) == function_bits(expected));
    assert(ptrauth_nop_cast(MCode *, lj_ptr_strip(actual)) ==
	   pair->child_mcode);
  }
#endif
  arena = lj_arena_of(pair->child);
  assert(arena != NULL);
  assert(lj_arena_lifetime_state_acq(arena,
	 lj_arena_cellof(pair->child)) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(arena,
	 lj_arena_cellof(pair->child)) == LJ_ARENA_ROOT_MEMBER);

  assert(trace_nchild_acq(pair->root) == 1);
  assert(trace_nextside_acq(pair->root) == pair->childno);
  assert(snap_topslot_acq(&pair->root_snap[pair->exitno]) ==
	 trace_topslot_acq(pair->root));
  expect_edge(g, pair, pair->child_mcode);
  assert(traceref_safe(J, pair->childno) == pair->child);
#ifdef LJ_TRACE_TEST_HELPERS
  if (pair->require_forl_root) {
    const uint32_t guard_mutations = (1u<<13)|(1u<<14)|(1u<<15)|
	(1u<<16)|(1u<<17)|(1u<<18);
    assert((lj_trace_test_arm64_side_scalar_copy_contract(J, pair->childno) &
	    guard_mutations) == guard_mutations);
  }
#endif
}

static void expect_native_child(lua_State *L, jit_State *J,
	const ProductionPair *pair)
{
  uint32_t before = live_trace_count(J);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_pair(L, pair, 2) == pair->native_result);
  if (lj_trace_test_exit_calls() == 0 ||
	lj_trace_test_first_exit_parent() != pair->childno ||
	lj_trace_test_last_exit_parent() != pair->childno)
    fprintf(stderr,
      "native child %s: calls=%u first=%u/%u last=%u/%u child=%u "
      "entries=%u/%u cleanup=%u\n",
      pair->name, (unsigned)lj_trace_test_exit_calls(),
      (unsigned)lj_trace_test_first_exit_parent(),
      (unsigned)lj_trace_test_first_exitno(),
      (unsigned)lj_trace_test_last_exit_parent(),
      (unsigned)lj_trace_test_last_exitno(), (unsigned)pair->childno,
      (unsigned)lj_trace_test_root_entry_publishes(),
      (unsigned)lj_trace_test_root_entry_successes(),
      (unsigned)lj_trace_test_root_entry_cleanups());
  assert(lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_exit_calls() <= pair->native_n);
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_first_exit_parent() == pair->childno);
  assert(lj_trace_test_last_exit_parent() == pair->childno);
  assert(lj_trace_test_first_exitno() < trace_nsnap_acq(pair->child));
  assert(lj_trace_test_last_exitno() < trace_nsnap_acq(pair->child));
  assert(live_trace_count(J) == before);
  assert(trace_nchild_acq(pair->child) == 0);
  assert(trace_nextside_acq(pair->child) == 0);
}

static void expect_forl_intmax_boundary(lua_State *L,
	const ProductionPair *pair)
{
  const lua_Integer start = (lua_Integer)INT32_MAX-1;
  const lua_Integer stop = INT32_MAX;
  lua_Integer safe_expected;
  lua_Integer expected = call_forl_named(L, pair->oracle_name, start, stop, 2);
  assert(expected == -(lua_Integer)2*INT32_MAX+1);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_named(L, pair->name, start, stop, 2) == expected);
  /* The invalid stop+step range is routed through the already-validated root
  ** exit and interpreter. This is whole-loop boundary evidence, not proof
  ** that the child-local overflow guard itself executed. */
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_first_exit_parent() == pair->rootno);
  assert(lj_trace_test_last_exit_parent() == pair->rootno);
  assert(lj_trace_test_first_exitno() < trace_nsnap_acq(pair->root));
  assert(lj_trace_test_last_exitno() < trace_nsnap_acq(pair->root));
  assert(trace_nchild_acq(pair->root) == 1);
  assert(trace_nextside_acq(pair->root) == pair->childno);
  safe_expected = call_forl_named(L, pair->oracle_name, start-1, stop-1, 2);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_named(L, pair->name, start-1, stop-1, 2) == safe_expected);
  /* stop=INT32_MAX-1 satisfies the child guard's stop+step bound. The safe
  ** near-boundary operation enters the attached child and exits at its
  ** ordinary in-body snapshot. */
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_first_exit_parent() == pair->childno);
  assert(lj_trace_test_last_exit_parent() == pair->childno);
  assert(lj_trace_test_first_exitno() < trace_nsnap_acq(pair->child));
  assert(lj_trace_test_last_exitno() < trace_nsnap_acq(pair->child));
}

static void expect_forl_intmin_boundary(lua_State *L,
	const ProductionPair *pair)
{
  const lua_Integer start = (lua_Integer)INT32_MIN+1;
  const lua_Integer stop = INT32_MIN;
  lua_Integer expected = call_forl_named(L, pair->oracle_name, start, stop, 2);
  assert(expected == -((lua_Integer)2*INT32_MIN+1));
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_forl_named(L, pair->name, start, stop, 2) == expected);
  /* The underflowing stop+step range exits through the root before a child
  ** entry; interpreter execution preserves the exact Lua integer result. */
  assert(lj_trace_test_root_entry_publishes() > 0);
  assert(lj_trace_test_root_entry_successes() > 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_first_exit_parent() == pair->rootno);
  assert(lj_trace_test_last_exit_parent() == pair->rootno);
  assert(lj_trace_test_first_exitno() < trace_nsnap_acq(pair->root));
  assert(lj_trace_test_last_exitno() < trace_nsnap_acq(pair->root));
  assert(trace_nchild_acq(pair->root) == 1);
  assert(trace_nextside_acq(pair->root) == pair->childno);
  {
    lua_Integer safe_start = start+1;
    lua_Integer safe_stop = stop+1;
    lua_Integer safe_expected = call_forl_named(L, pair->oracle_name,
		safe_start, safe_stop, 2);
    assert(safe_expected == -((lua_Integer)2*INT32_MIN+3));
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_forl_named(L, pair->name, safe_start, safe_stop, 2) ==
	   safe_expected);
    /* stop=INT32_MIN+1 satisfies stop+step >= INT32_MIN. */
    assert(lj_trace_test_root_entry_publishes() > 0);
    assert(lj_trace_test_root_entry_successes() > 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_exit_calls() > 0);
    assert(lj_trace_test_first_exit_parent() == pair->childno);
    assert(lj_trace_test_last_exit_parent() == pair->childno);
    assert(lj_trace_test_first_exitno() < trace_nsnap_acq(pair->child));
    assert(lj_trace_test_last_exitno() < trace_nsnap_acq(pair->child));
  }
}

static void expect_unsupported_first_side_closed(lua_State *L, jit_State *J,
	global_State *g, ProductionPair *pair)
{
  record_root(L, J, pair);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_root_acq(pair->root) == 0);
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  set_hotexit(L, 1);
  lj_trace_test_reset_exittab_stats();
  for (unsigned attempt = 0; attempt < 128; attempt++)
    assert(call_pair(L, pair, 1) == pair->side_result);
  assert(lj_trace_test_abort_count() >= 1);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  UNUSED(g);
}


static void expect_quiescent(lua_State *L, jit_State *J, global_State *g,
	TGState *tg, void *saved_cframe, int32_t saved_vmstate)
{
  assert(J->curfinal == NULL);
  assert(trace_exittab_acq(&J->cur) == NULL);
  assert(trace_exitstub_acq(&J->cur) == NULL);
  assert(trace_traceno_acq(&J->cur) == 0);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(side_parent_cert_zero(&J->arm64_side_parent));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_IDLE);
}

static void expect_gc_claim(jit_State *J, global_State *g,
	ProductionPair *pairs)
{
  unsigned i, j;
  lj_trace_test_reset_retire_publish_calls();
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    uint64_t stamp;
    assert(lj_trace_retire_gc_claim(g, pair->child) == 1);
    stamp = la_load64_acq(&pair->child->retire_epoch);
    assert(stamp != 0);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_native_pin_closed_acq(pair->child));
    assert(trace_native_pins_acq(pair->child) == 0);
    assert(!trace_runnable_acq(pair->child, pair->childno));
    assert(traceref_safe(J, pair->childno) == pair->child);
    assert(trace_traceno_acq(pair->child) == pair->childno);
    assert(trace_root_acq(pair->child) == pair->rootno);
    assert(trace_link_acq(pair->child) == 0);
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    expect_edge(g, pair, pair->root_fallback);
    assert(lj_trace_retire_gc_claim(g, pair->child) == 1);
    assert(la_load64_acq(&pair->child->retire_epoch) == stamp);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    for (j = i+1u; j < PRODUCTION_PAIR_COUNT; j++)
      expect_edge(g, &pairs[j], pairs[j].child_mcode);
  }
}

static void expect_scoped(jit_State *J, global_State *g,
	ProductionPair *pairs)
{
  unsigned i, j;
  lj_trace_test_reset_retire_publish_calls();
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    assert(lj_trace_flushscope(J, pair->childno) == 1u);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    assert(la_load64_acq(&pair->child->retire_epoch) != 0);
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_native_pin_closed_acq(pair->child));
    assert(trace_native_pins_acq(pair->child) == 0);
    assert(trace_traceno_acq(pair->child) == 0);
    assert(trace_root_acq(pair->child) == pair->rootno);
    assert(trace_link_acq(pair->child) == 0);
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    assert(trace_runnable_acq(pair->root, pair->rootno));
    assert(proto_trace_acq(pair->pt) == pair->rootno);
    expect_edge(g, pair, pair->root_fallback);
    for (j = i+1u; j < PRODUCTION_PAIR_COUNT; j++)
      expect_edge(g, &pairs[j], pairs[j].child_mcode);
  }
}

static void expect_full_flush(lua_State *L, jit_State *J, global_State *g,
	GCproto *decoy_pt, GCtrace *decoy, ProductionPair *pairs,
	ProductionPair *unsupported)
{
  unsigned i;
  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_flushall_gc(L) == 0);
  assert(lj_trace_test_retire_publish_calls() ==
	 2u*PRODUCTION_PAIR_COUNT+2u);
  assert(trace_traceno_acq(decoy) == 0);
  assert(proto_trace_acq(decoy_pt) == 0);
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    assert(trace_traceno_acq(pair->root) == 0);
    assert(trace_traceno_acq(pair->child) == 0);
    assert(la_load64_acq(&pair->root->retire_epoch) != 0);
    assert(la_load64_acq(&pair->child->retire_epoch) != 0);
    assert(trace_retired_link_listed_acq(pair->root));
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    assert(trace_link_acq(pair->child) == 0);
    assert(proto_trace_acq(pair->pt) == 0);
    expect_edge(g, pair, pair->root_fallback);
  }
  assert(trace_traceno_acq(unsupported->root) == 0);
  assert(proto_trace_acq(unsupported->pt) == 0);
  assert(trace_nchild_acq(unsupported->root) == 0);
  assert(trace_nextside_acq(unsupported->root) == 0);
  assert(J->mcarea == NULL && J->mctop == NULL && J->mcbot == NULL);
}

static int detailed_main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "gc-claim";
  lua_State *L = luaL_newstate();
  jit_State *J;
  global_State *g;
  TGState *tg;
  GCproto *decoy_pt;
  GCtrace *decoy;
  TraceNo decoy_no = 0;
  ProductionPair pairs[PRODUCTION_PAIR_COUNT] = {
    {
      .name = "__arm64_first_side_production_first",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 3, .side_bias = 1, .side_result = 4,
      .native_n = 3, .native_bias = 1, .native_result = 4,
      .exitno = 2
    },
    {
      .name = "__arm64_first_side_production_second",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 3, .side_bias = 0, .side_result = 3,
      .native_n = 2, .native_bias = 0, .native_result = 2,
      .exitno = 6
    },
    {
      .name = "__arm64_first_side_production_third",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 4, .side_bias = 0, .side_result = 4,
      .native_n = 3, .native_bias = 0, .native_result = 3,
      .exitno = 7
    },
    {
      .name = "__arm64_first_side_production_fourth",
      .root_n = 3, .root_bias = 0, .root_result = 4,
      .side_n = 5, .side_bias = 0, .side_result = 6,
      .native_n = 3, .native_bias = 0, .native_result = 4,
      .exitno = 6
    },
    {
      .name = "__arm64_first_side_production_loopchild",
      .root_n = 64, .root_bias = 1, .root_result = 64,
      .side_n = 64, .side_bias = 1.25, .side_result = 80,
      .native_n = 64, .native_bias = 1.25, .native_result = 80,
      .require_loop_child = 1
    },
    {
      .name = "__arm64_first_side_production_forlchild",
      .root_n = 64, .root_bias = 1, .root_result = 2080,
      .side_n = 64, .side_bias = 2, .side_result = -2080,
      .native_n = 64, .native_bias = 2, .native_result = -2080,
      .root_start = 1, .side_start = 1, .native_start = 1,
      .require_forl_root = 1,
      .oracle_name = "__arm64_first_side_production_forl_oracle"
    },
    {
      .name = "__arm64_first_side_production_forlchildneg",
      .root_n = 1, .root_bias = 1, .root_result = 2080,
      .side_n = 1, .side_bias = 2, .side_result = -2080,
      .native_n = 1, .native_bias = 2, .native_result = -2080,
      .root_start = 64, .side_start = 64, .native_start = 64,
      .require_forl_root = 1,
      .oracle_name = "__arm64_first_side_production_forlneg_oracle"
    }
  };
  ProductionPair unsupported = {
    .name = "__arm64_first_side_production_unsupported",
    .root_n = 4, .root_bias = 0, .root_result = 6,
    .side_n = 7, .side_bias = 1, .side_result = 9,
    .exitno = 6
  };
  void *saved_cframe;
  int32_t saved_vmstate;
  unsigned i, j;

  assert(argc <= 2);
  assert(strcmp(mode, "gc-claim") == 0 ||
	 strcmp(mode, "scoped") == 0 ||
	 strcmp(mode, "full-flush") == 0);
  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  g = G(L);
  tg = L2TG(L);
  assert(J != NULL && g != NULL && tg != NULL);
  define_probes(L);
  run_lua(L,
	"local function f(start,n,mode) local sum=0; "
	"for i=start,n do if mode==1 then sum=sum+i "
	"elseif mode==2 then sum=sum-i else sum=sum+i*3 end end; "
    "return sum end; "
    "local function fneg(start,n,mode) local sum=0; "
    "for i=start,n,-1 do if mode==1 then sum=sum+i "
    "elseif mode==2 then sum=sum-i else sum=sum+i*3 end end; "
    "return sum end; jit.off(f,true); jit.off(fneg,true); "
    "__arm64_first_side_production_forl_oracle=f; "
    "__arm64_first_side_production_forlneg_oracle=fneg");
  start_live_peer(L);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  decoy_pt = named_proto(L, "__arm64_first_side_production_decoy");
  assert(call_named(L, "__arm64_first_side_production_decoy", 3, 0) == 3);
  decoy = find_root(J, decoy_pt, &decoy_no);
  assert(decoy != NULL && decoy_no != 0);

  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    record_root(L, J, &pairs[i]);
    expect_root_shape(J, g, &pairs[i]);
    assert(pairs[i].rootno > (i == 0 ? decoy_no : pairs[i-1u].childno));
    set_hotexit(L, 10);
    expect_post_token_request_cleanup(L, J, g, tg, &pairs[i]);
    record_child(L, J, &pairs[i]);
    expect_child_shape(J, g, &pairs[i]);
    for (j = 0; j < i; j++) {
      assert(pairs[i].rootno != pairs[j].rootno);
      assert(pairs[i].rootno != pairs[j].childno);
      assert(pairs[i].childno != pairs[j].childno);
    }
  }
  expect_unsupported_first_side_closed(L, J, g, &unsupported);
  assert(unsupported.rootno > pairs[PRODUCTION_PAIR_COUNT-1u].childno);
  assert(live_trace_count(J) == 2u*PRODUCTION_PAIR_COUNT+2u);

  expect_quiescent(L, J, g, tg, saved_cframe, saved_vmstate);
  for (j = 0; j < 2; j++)
    for (i = 0; i < PRODUCTION_PAIR_COUNT; i++)
      expect_native_child(L, J, &pairs[i]);
  expect_forl_intmax_boundary(L, &pairs[PRODUCTION_PAIR_COUNT-2u]);
  expect_forl_intmin_boundary(L, &pairs[PRODUCTION_PAIR_COUNT-1u]);
  assert(live_trace_count(J) == 2u*PRODUCTION_PAIR_COUNT+2u);
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++)
    expect_edge(g, &pairs[i], pairs[i].child_mcode);
  expect_quiescent(L, J, g, tg, saved_cframe, saved_vmstate);

  if (strcmp(mode, "full-flush") == 0)
    expect_full_flush(L, J, g, decoy_pt, decoy, pairs, &unsupported);
  else if (strcmp(mode, "scoped") == 0)
    expect_scoped(J, g, pairs);
  else
    expect_gc_claim(J, g, pairs);

  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  stop_live_peer(L);
  lua_close(L);
  printf("t-arm64-jit-first-side-production %s OK\n", mode);
  return 0;
}

#endif

int main(int argc, char **argv)
{
#ifdef LJ_TRACE_TEST_HELPERS
  return detailed_main(argc, argv);
#else
  (void)argc;
  (void)argv;
  return smoke_main();
#endif
}

#else

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  puts("t-arm64-jit-first-side-production SKIP");
  return 0;
}

#endif
