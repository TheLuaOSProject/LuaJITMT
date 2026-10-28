/* Independent source/IR and post-RA checks for immutable GCstr length loads. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_obj.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"

#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__arm64__)) || \
    !defined(LUAJIT_MT_ARM64_BOOTSTRAP) || \
    !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !defined(LJ_TRACE_TEST_HELPERS)
#error "string-length admission requires experimental native ARM64"
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
    fprintf(stderr, "string-length Lua fixture: %s\n",
            lua_tostring(L, -1));
    abort();
  }
}

static lua_Number call_loop(lua_State *L, int table_arg)
{
  lua_getglobal(L, "string_len_trace");
  if (table_arg) {
    int i;
    lua_newtable(L);
    for (i = 1; i <= 4; i++) {
      lua_pushinteger(L, i);
      lua_rawseti(L, -2, i);
    }
  } else {
    lua_getglobal(L, "string_len_value");
  }
  lua_pushinteger(L, 80);
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    fprintf(stderr, "string-length call failed: %s\n",
            lua_tostring(L, -1));
    abort();
  }
  assert(lua_isnumber(L, -1));
  {
    lua_Number result = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return result;
  }
}

int main(void)
{
  static const char source[] =
    "package.path='./src/?.lua;./src/?/init.lua;'..package.path\n"
    "local jit=require('jit')\n"
    "assert(jit.status(),'JIT is required')\n"
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1')\n"
    "string_len_trace=function(s,n)\n"
    " local i,sum=1,0\n"
    " while i<=n do sum=sum+#s; i=i+1 end\n"
    " return sum\n"
    "end\n"
    "string_len_trace_clone=function(s,n)\n"
    " local i,sum=1,0\n"
    " while i<=n do sum=sum+#s; i=i+1 end\n"
    " return sum\n"
    "end\n"
    "string_len_value=string.rep('gcstr-length-',32)\n"
    "assert(string_len_trace(string_len_value,80)=="
    "#string_len_value*80)\n";
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  GCtrace *owner;
  const IRIns *ir;
  IRRef r, fload = 0, source_ref;
  MSize s;
  unsigned mapped_source = 0;
  uint32_t entries, exits;
  assert(L != NULL);
  luaL_openlibs(L);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  run_chunk(L, source);

  assert(lj_trace_test_root_entry_successes() > 0);
  owner = traceref_safe(L2J(L), 1);
  assert(owner && trace_runnable_acq(owner, 1));
  assert(bc_op(trace_startins_acq(owner)) == BC_LOOP);
  assert(trace_mcode_acq(owner) != NULL &&
         trace_szmcode_acq(owner) > sizeof(MCode));
  ir = trace_ir_acq(owner);
  for (r = REF_FIRST; r < trace_nins_acq(owner); r++) {
    if (ir[r].o == IR_FLOAD) {
      assert(ir[r].t.irt == IRT_INT && ir[r].op2 == IRFL_STR_LEN);
      if (!fload) fload = r;
    }
  }
  assert(fload != 0);
  source_ref = ir[fload].op1;
  assert(source_ref >= REF_FIRST && source_ref < fload &&
         ir[source_ref].o == IR_SLOAD &&
         ir[source_ref].t.irt == (IRT_STR|IRT_GUARD) &&
         ir[source_ref].op2 == IRSLOAD_TYPECHECK);
  for (s = 0; s < trace_nsnap_acq(owner); s++) {
    const SnapShot *snap = &trace_snap_acq(owner)[s];
    MSize i;
    for (i = 0; i < snap_nent_acq(snap); i++) {
      SnapEntry entry = snapentry_acq(
        &trace_snapmap_acq(owner)[snap_mapofs_acq(snap)+i]);
      if (snap_ref(entry) == source_ref) {
        assert(!(entry & (SNAP_NORESTORE|SNAP_FRAME)) &&
               snap_slot(entry) < snap_nslots_acq(snap));
        mapped_source++;
      }
    }
  }
  assert(mapped_source > 0);

  /* A full collection runs while the dynamic string remains in a Lua local
  ** and must not invalidate the native length load or prevent reentry. */
  run_chunk(L, "collectgarbage('collect')");
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, 0) == (lua_Number)(sizeof("gcstr-length-")-1u)*32u*80u);
  assert(lj_trace_test_root_entry_successes() > entries);
  exits = lj_trace_test_exit_calls();
  assert(call_loop(L, 1) == 320.0);
  assert(lj_trace_test_exit_calls() > exits);

  arm64_fixture_init(&f, L, "string_len_trace_clone");
  assert(semantic(&f) && allocated(&f));
  fload = 0;
  for (r = REF_FIRST; r < f.semantic_nins; r++)
    if (f.ir[r].o == IR_FLOAD && f.ir[r].op2 == IRFL_STR_LEN) {
      fload = r;
      break;
    }
  assert(fload != 0);
  source_ref = f.ir[fload].op1;

#define REJECT_BOTH(change) do { \
  int sem_ok, post_ok; \
  arm64_fixture_reset(&f); \
  assert(semantic(&f) && allocated(&f)); \
  change; \
  sem_ok = semantic(&f); post_ok = allocated(&f); \
  if (sem_ok || post_ok) { \
    fprintf(stderr, "string-length mutation accepted at line %d " \
                    "(semantic=%d postRA=%d)\n", \
            __LINE__, sem_ok, post_ok); \
    abort(); \
  } \
} while (0)
  REJECT_BOTH(f.ir[fload].op2 = IRFL_TAB_ASIZE;
              f.postra[fload].op2 = IRFL_TAB_ASIZE);
  REJECT_BOTH(f.ir[fload].t.irt = IRT_NUM;
              f.postra[fload].t.irt = IRT_NUM);
  REJECT_BOTH(f.ir[fload].t.irt |= IRT_GUARD;
              f.postra[fload].t.irt |= IRT_GUARD);
  REJECT_BOTH(f.ir[fload].op1 = fload;
              f.postra[fload].op1 = fload);
  REJECT_BOTH(f.ir[fload].op1 = REF_NIL;
              f.postra[fload].op1 = REF_NIL);
  REJECT_BOTH(f.ir[source_ref].t.irt = IRT_NUM|IRT_GUARD;
              f.postra[source_ref].t.irt = IRT_NUM|IRT_GUARD);
  REJECT_BOTH(f.ir[source_ref].op1 = f.view.root_topslot + 1u;
              f.postra[source_ref].op1 = f.view.root_topslot + 1u);
#undef REJECT_BOTH

  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  lua_close(L);
  printf("string length: FLOAD=%u mapped=%u native=%u exits=%u\n",
         (unsigned)fload, mapped_source,
         lj_trace_test_root_entry_successes(), lj_trace_test_exit_calls());
  puts("t-arm64-jit-string-len-admission OK");
  return 0;
}
