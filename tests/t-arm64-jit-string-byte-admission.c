/* Independent semantic and post-RA admission tests for bounded byte loads. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"

#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__arm64__)) || \
    !defined(LUAJIT_MT_ARM64_BOOTSTRAP) || \
    !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !defined(LJ_TRACE_TEST_HELPERS)
#error "string-byte admission requires experimental native ARM64"
#endif

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &reject);
}

static int allocated(Arm64TraceFixture *f)
{
  IRRef end;
  return lj_asm_arm64_postra_admit(&f->view, &end);
}

static void run_chunk(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    fprintf(stderr, "string-byte Lua fixture: %s\n",
            lua_tostring(L, -1));
    abort();
  }
}

static int call_loop(lua_State *L, const char *function_global,
                     const char *value_global, lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  if (value_global) {
    lua_getglobal(L, value_global);
  } else {
    int i;
    lua_newtable(L);
    for (i = 1; i <= 4; i++) {
      lua_pushinteger(L, i);
      lua_rawseti(L, -2, i);
    }
  }
  lua_pushinteger(L, 80);
  status = lua_pcall(L, 2, 1, 0);
  if (status == LUA_OK) {
    assert(lua_isnumber(L, -1));
    *out = lua_tonumber(L, -1);
  } else {
    assert(lua_isstring(L, -1));
  }
  lua_pop(L, 1);
  return status;
}

static int kint(const IRIns *ir, IRRef nk, IRRef ref, int32_t *value)
{
  IRRef r;
  if (ref < nk || ref >= REF_TRUE) return 0;
  for (r = nk; r < REF_TRUE; r++) {
    if (r == ref) {
      if (ir[r].o != IR_KINT || ir[r].t.irt != IRT_INT) return 0;
      *value = ir[r].i;
      return 1;
    }
    if (ir[r].o == IR_KNUM || ir[r].o == IR_KGC) r++;
  }
  return 0;
}

int main(void)
{
  static const char source[] =
    "package.path='./src/?.lua;./src/?/init.lua;'..package.path\n"
    "local jit=require('jit')\n"
    "assert(jit.status(),'JIT is required')\n"
    "local threading=require('threading')\n"
    "string_byte_ready=threading.channel(1)\n"
    "string_byte_stop=threading.channel(1)\n"
    "string_byte_peer=assert(threading.spawn(function(ready,stop)\n"
    " assert(ready:send(true,5)); assert(select(2,stop:recv(120))); "
    " return true end,string_byte_ready,string_byte_stop))\n"
    "assert(select(2,string_byte_ready:recv(5)),'peer failed to start')\n"
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=4')\n"
    "string_byte_trace=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte(s,2) end; return sum\n"
    "end\n"
    "string_byte_trace_clone=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte(s,2) end; return sum\n"
    "end\n"
    "string_byte_bounds_trace=function(s,n)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,2); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_value=string.rep('gcstr-byte-',24)\n"
    "string_byte_nul='a\\0b'\n"
    "string_byte_short='x'\n"
    "string_byte_empty=''\n"
    "string_byte_long=string.rep('L',65536)\n"
    "assert(string_byte_trace(string_byte_value,80)=="
    "string.byte('gcstr-byte-',2)*80)\n"
    "assert(string_byte_bounds_trace(string_byte_value,80)=="
    "string.byte('gcstr-byte-',2)*80+0.5)\n";
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  GCtrace *owner, *bounds_owner;
  const IRIns *ir;
  IRRef r, source_ref = 0, len_ref = 0, guard_ref = 0;
  IRRef strref = 0, xload = 0;
  MSize s;
  unsigned mapped_source = 0;
  uint32_t entries, exits;
  TraceNo bounds_no;
  int sem_ok, post_ok;
  lua_Number value;
  assert(L != NULL);
  luaL_openlibs(L);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  run_chunk(L, source);

  if (lj_trace_test_root_entry_successes() == 0) {
    TraceNo primary_no, bounds_no;
    lua_getglobal(L, "string_byte_trace");
    primary_no = proto_trace_acq(funcproto(funcV(L->top-1)));
    lua_pop(L, 1);
    lua_getglobal(L, "string_byte_bounds_trace");
    bounds_no = proto_trace_acq(funcproto(funcV(L->top-1)));
    lua_pop(L, 1);
    fprintf(stderr, "string-byte no native root: publishes=%u cleanups=%u "
            "starts=%u aborts=%u error=%u exits=%u traceNos=%u,%u\n",
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(),
            lj_trace_test_root_entry_startins_calls(),
            lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            lj_trace_test_exit_calls(), (unsigned)primary_no,
            (unsigned)bounds_no);
  }
  assert(lj_trace_test_root_entry_successes() > 0);
  owner = traceref_safe(L2J(L), 1);
  if (!owner || !trace_runnable_acq(owner, 1))
    fprintf(stderr, "string-byte trace1 owner=%p traceno=%u retire=%llu "
            "entryflags=%u entries=%u\n", (void *)owner,
            owner ? (unsigned)trace_traceno_acq(owner) : 0u,
            owner ? (unsigned long long)la_load64_acq(&owner->retire_epoch) : 0,
            owner ? (unsigned)la_load8_acq(&owner->unused1) : 0u,
            lj_trace_test_root_entry_successes());
  assert(owner && trace_runnable_acq(owner, 1));
  assert(bc_op(trace_startins_acq(owner)) == BC_FORL);
  assert(trace_mcode_acq(owner) != NULL &&
         trace_szmcode_acq(owner) > sizeof(MCode));
  ir = trace_ir_acq(owner);
  for (r = REF_FIRST; r < trace_nins_acq(owner); r++) {
    if (ir[r].o == IR_SLOAD && ir[r].t.irt == (IRT_STR|IRT_GUARD) &&
        ir[r].op2 == IRSLOAD_TYPECHECK)
      source_ref = r;
    if (ir[r].o == IR_FLOAD && ir[r].t.irt == IRT_INT &&
        ir[r].op2 == IRFL_STR_LEN)
      len_ref = r;
    if (ir[r].o == IR_STRREF) strref = r;
    if (ir[r].o == IR_XLOAD) xload = r;
  }
  assert(source_ref && len_ref && strref && xload);
  assert(ir[len_ref].op1 == source_ref);
  assert(ir[strref].op1 == source_ref);
  assert(ir[xload].op1 == strref && ir[xload].t.irt == IRT_U8 &&
         ir[xload].op2 == IRXLOAD_READONLY);
  {
    int32_t offset, bound;
    assert(kint(ir, trace_nk_acq(owner), ir[strref].op2, &offset) &&
           offset == 1);
    for (r = REF_FIRST; r < strref; r++) {
      IRRef length = 0, limit = 0;
      if (ir[r].o == IR_UGE) length = ir[r].op1, limit = ir[r].op2;
      if (ir[r].o == IR_ULE) length = ir[r].op2, limit = ir[r].op1;
      if (ir[r].t.irt == (IRT_INT|IRT_GUARD) && length == len_ref &&
          kint(ir, trace_nk_acq(owner), limit, &bound) && bound == offset+1)
        guard_ref = r;
    }
    assert(guard_ref != 0);
  }
  for (s = 0; s < trace_nsnap_acq(owner); s++) {
    const SnapShot *snap = &trace_snap_acq(owner)[s];
    MSize i;
    for (i = 0; i < snap_nent_acq(snap); i++) {
      SnapEntry entry = snapentry_acq(
        &trace_snapmap_acq(owner)[snap_mapofs_acq(snap)+i]);
      assert(snap_ref(entry) != strref);
      assert(snap_ref(entry) != xload);
      if (snap_ref(entry) == source_ref) {
        assert(!(entry & (SNAP_NORESTORE|SNAP_FRAME)) &&
               snap_slot(entry) < snap_nslots_acq(snap));
        mapped_source++;
      }
    }
  }
  assert(mapped_source > 0);
  lua_getglobal(L, "string_byte_bounds_trace");
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  bounds_no = proto_trace_acq(funcproto(funcV(L->top-1)));
  bounds_owner = traceref_safe(L2J(L), bounds_no);
  lua_pop(L, 1);
  assert(bounds_no != 0 && bounds_owner &&
         trace_runnable_acq(bounds_owner, bounds_no));
  assert(bc_op(trace_startins_acq(bounds_owner)) == BC_LOOP);

  /* Hold a second TG live while collection and all successful byte loads run. */
  run_chunk(L, "collectgarbage('collect')");
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_trace", "string_byte_value", &value) == LUA_OK &&
         value == 99.0*80.0);
  assert(lj_trace_test_root_entry_successes() > entries);
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_trace", "string_byte_nul", &value) == LUA_OK && value == 0.0);
  assert(lj_trace_test_root_entry_successes() > entries);
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_trace", "string_byte_long", &value) == LUA_OK &&
         value == 76.0*80.0);
  assert(lj_trace_test_root_entry_successes() > entries);
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_bounds_trace", "string_byte_value",
                   &value) == LUA_OK && value == 99.0*80.0+0.5);
  assert(lj_trace_test_root_entry_successes() > entries);
  /* A LOOP begins before its body, so bad bounds and type inputs execute its
  ** certified native guard before interpreter bytecode raises the error. */
  exits = lj_trace_test_exit_calls();
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_bounds_trace", "string_byte_short", &value) != LUA_OK);
  assert(lj_trace_test_root_entry_successes() > entries);
  assert(lj_trace_test_exit_calls() > exits);
  exits = lj_trace_test_exit_calls();
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_bounds_trace", "string_byte_empty", &value) != LUA_OK);
  assert(lj_trace_test_root_entry_successes() > entries);
  assert(lj_trace_test_exit_calls() > exits);
  exits = lj_trace_test_exit_calls();
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_bounds_trace", NULL, &value) != LUA_OK);
  assert(lj_trace_test_root_entry_successes() > entries);
  assert(lj_trace_test_exit_calls() > exits);

  arm64_fixture_init(&f, L, "string_byte_trace_clone");
  assert(semantic(&f) && allocated(&f));
  strref = xload = source_ref = len_ref = guard_ref = 0;
  for (r = REF_FIRST; r < f.semantic_nins; r++) {
    if (f.ir[r].o == IR_STRREF) strref = r;
    if (f.ir[r].o == IR_XLOAD) xload = r;
    if (f.ir[r].o == IR_SLOAD && f.ir[r].t.irt == (IRT_STR|IRT_GUARD))
      source_ref = r;
    if (f.ir[r].o == IR_FLOAD && f.ir[r].op2 == IRFL_STR_LEN)
      len_ref = r;
  }
  assert(strref && xload && source_ref && len_ref);
  for (r = REF_FIRST; r < strref; r++)
    if ((f.ir[r].o == IR_UGE && f.ir[r].op1 == len_ref) ||
        (f.ir[r].o == IR_ULE && f.ir[r].op2 == len_ref))
      guard_ref = r;
  assert(guard_ref);

#define REJECT_BOTH(change) do { \
  int sem_ok, post_ok; \
  arm64_fixture_reset(&f); \
  assert(semantic(&f) && allocated(&f)); \
  change; \
  sem_ok = semantic(&f); post_ok = allocated(&f); \
  if (sem_ok || post_ok) { \
    fprintf(stderr, "string-byte mutation accepted at line %d " \
                    "(semantic=%d postRA=%d)\\n", \
            __LINE__, sem_ok, post_ok); \
    abort(); \
  } \
} while (0)
#define REJECT_POSTRA(change) do { \
  arm64_fixture_reset(&f); \
  assert(semantic(&f) && allocated(&f)); \
  change; \
  assert(semantic(&f)); \
  if (allocated(&f)) { \
    fprintf(stderr, "string-byte postRA mutation accepted at line %d\n", \
            __LINE__); \
    abort(); \
  } \
} while (0)
  REJECT_BOTH(f.ir[xload].t.irt = IRT_I8;
              f.postra[xload].t.irt = IRT_I8);
  REJECT_BOTH(f.ir[xload].op2 = IRXLOAD_VOLATILE;
              f.postra[xload].op2 = IRXLOAD_VOLATILE);
  REJECT_BOTH(f.ir[xload].op1 = REF_NIL;
              f.postra[xload].op1 = REF_NIL);
  REJECT_BOTH(f.ir[strref].op2 = REF_NIL;
              f.postra[strref].op2 = REF_NIL);
  REJECT_BOTH(f.ir[guard_ref].op2 = f.ir[guard_ref].op1;
              f.postra[guard_ref].op2 = f.postra[guard_ref].op1);
  REJECT_BOTH(f.ir[len_ref].op1 = REF_NIL;
              f.postra[len_ref].op1 = REF_NIL);
  /* Replace the exact STRREF with a stack TMPREF that is otherwise a
  ** legitimate PGC producer; byte-load admission must keep its own provenance. */
  REJECT_BOTH(f.ir[strref].o = IR_TMPREF;
              f.ir[strref].t.irt = IRT_PGC;
              f.ir[strref].op1 = source_ref;
              f.ir[strref].op2 = IRTMPREF_IN2;
              f.postra[strref].o = IR_TMPREF;
              f.postra[strref].t.irt = IRT_PGC;
              f.postra[strref].op1 = source_ref;
              f.postra[strref].op2 = IRTMPREF_IN2;
              f.postra[strref].r = RID_X0;
              f.postra[strref].s = SPS_NONE);
  REJECT_POSTRA(f.postra[strref].r = RID_D0);
  REJECT_POSTRA(f.postra[xload].r = RID_D0);
  REJECT_POSTRA(f.postra[source_ref].r = RID_D0);
  REJECT_POSTRA(f.postra[source_ref].r = RID_INIT;
                f.postra[source_ref].s = SPS_NONE);
#undef REJECT_BOTH
#undef REJECT_POSTRA

  arm64_fixture_reset(&f);
  {
    IRRef phi = 0, loop = 0;
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      if (!loop && f.ir[r].o == IR_LOOP) loop = r;
      if (!phi && f.ir[r].o == IR_PHI) phi = r;
    }
    assert(phi && loop);
    f.postra[phi].op1 = strref;
    assert(!allocated(&f));
  }

  /* A byte load is zero-extended for arithmetic, but U8 is not a snapshot
  ** TValue encoding. Both validators must reject direct snapshot escape. */
  {
    MSize sn, mapidx = 0;
    int found = 0;
    SnapEntry old;
    arm64_fixture_reset(&f);
    for (sn = 0; sn < f.view.nsnap && !found; sn++) {
      const SnapShot *snap = &f.snaps[sn];
      MSize i;
      if (snap_ref_acq(snap) <= xload) continue;
      for (i = 0; i < snap_nent_acq(snap); i++) {
        MSize candidate = snap_mapofs_acq(snap)+i;
        if (snap_ref(f.map[candidate]) == source_ref) {
          mapidx = candidate;
          found = 1;
          break;
        }
      }
    }
    assert(found);
    old = f.map[mapidx];
    f.map[mapidx] = snap_setref(old, xload);
    f.postmap[mapidx] = snap_setref(old, xload);
    assert(f.semantic.snapmap == f.map &&
           snap_ref(f.semantic.snapmap[mapidx]) == xload);
    sem_ok = semantic(&f);
    post_ok = allocated(&f);
    f.map[mapidx] = old;
    f.postmap[mapidx] = old;
    if (sem_ok || post_ok) {
      fprintf(stderr, "string-byte U8 snapshot mutation accepted "
              "(semantic=%d postRA=%d) mapidx=%u xload=%u "
              "snapref=%u mapped=%u opcode=%u type=%u\n", sem_ok,
              post_ok, (unsigned)mapidx, (unsigned)xload,
              (unsigned)snap_ref_acq(&f.snaps[sn-1u]),
              (unsigned)snap_ref(old), (unsigned)f.ir[xload].o,
              (unsigned)f.ir[xload].t.irt);
    }
    assert(!sem_ok && !post_ok);
  }

  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  run_chunk(L, "assert(string_byte_stop:send(true,5)); "
               "local joined,result=string_byte_peer:join(5); "
               "assert(joined and result)");
  lua_close(L);
  printf("string byte: STRREF=%u XLOAD=%u source_snapshots=%u "
         "native=%u exits=%u\n", (unsigned)strref, (unsigned)xload,
         mapped_source, lj_trace_test_root_entry_successes(),
         lj_trace_test_exit_calls());
  puts("t-arm64-jit-string-byte-admission OK");
  return 0;
}
