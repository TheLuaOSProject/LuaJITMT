/* Native and source-identity checks for the string-input tostring fast path. */
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
#include "lj_asm.h"
#include "lj_ff.h"
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"

#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__arm64__)) || \
    !defined(LUAJIT_MT_ARM64_BOOTSTRAP) || \
    !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) || \
    !defined(LJ_TRACE_TEST_HELPERS)
#error "tostring admission requires experimental native ARM64"
#endif

extern uint32_t lj_trace_test_root_entry_successes(void);
extern void lj_trace_test_reset_exit_stats(void);
extern uint32_t lj_trace_test_exit_calls(void);
extern TraceNo lj_trace_test_first_exit_parent(void);

static void run_chunk(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    fprintf(stderr, "tostring fixture setup failed: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static void push_value(lua_State *L, int kind)
{
  switch (kind) {
  case 0: lua_pushlstring(L, "a\0b", 3); break;
  case 1: { const char value[] = { (char)0x80, 'x', 0 }; lua_pushlstring(L, value, 2); break; }
  case 2: lua_pushliteral(L, ""); break;
  case 3: lua_pushnumber(L, 2.75); break;
  case 4: lua_pushboolean(L, 1); break;
  case 5: lua_getglobal(L, "tostring_table"); break;
  case 6: lua_getglobal(L, "tostring_error_table"); break;
  default: abort();
  }
}

static int call_loop_status(lua_State *L, const char *fnname,
                            const char *callee, int kind,
                            const char **out, size_t *outlen)
{
  int status;
  lua_getglobal(L, fnname);
  lua_getglobal(L, callee);
  push_value(L, kind);
  lua_pushinteger(L, 40);
  status = lua_pcall(L, 3, 1, 0);
  *out = lua_tolstring(L, -1, outlen);
  if (status == LUA_OK) assert(*out != NULL);
  /* Results are copied before popping; callers compare immediately. */
  return status;
}

static void call_loop(lua_State *L, const char *fnname, const char *callee,
                      int kind, const char **out, size_t *outlen)
{
  int status = call_loop_status(L, fnname, callee, kind, out, outlen);
  if (status != LUA_OK) {
    fprintf(stderr, "%s failed: %s\n", fnname, *out ? *out : "(non-string error)");
    abort();
  }
}

static lua_Integer global_integer(lua_State *L, const char *name)
{
  lua_Integer value;
  lua_getglobal(L, name);
  value = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return value;
}

static int semantic_fixture(Arm64TraceFixture *f)
{
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &reject);
}

static int postra_fixture(Arm64TraceFixture *f)
{
  IRRef end;
  return lj_asm_arm64_postra_admit(&f->view, &end);
}

static IRRef tostring_guard_constant(const GCtrace *T)
{
  IRRef ref;
  for (ref = T->nk; ref < T->nins; ref++) {
    IRIns guard = ir_load_acq(&T->ir[ref]);
    IRRef constant;
    if (guard.o != IR_EQ || guard.t.irt != (IRT_FUNC|IRT_GUARD)) continue;
    constant = irref_isk(guard.op1) ? guard.op1 : guard.op2;
    if (constant >= T->nk && constant < REF_TRUE &&
        T->ir[constant].o == IR_KGC &&
        T->ir[constant].t.irt == IRT_FUNC &&
        lj_func_ffid_acq(gco2func(ir_kgc_acq(&T->ir[constant]))) == FF_tostring)
      return constant;
  }
  return 0;
}

static void test_wrong_builtin_constant(lua_State *L, const char *clone_name,
                                        TraceNo no)
{
  Arm64TraceFixture f;
  IRRef constant;
  GCfunc *wrong;
  lua_getglobal(L, "string");
  lua_getfield(L, -1, "byte");
  assert(lua_isfunction(L, -1) && isffunc(funcV(L->top-1)));
  wrong = funcV(L->top-1);
  lua_pop(L, 2);
  if (no == 1)
    arm64_fixture_init(&f, L, clone_name);
  else
    arm64_fixture_init_trace(&f, L, clone_name, no);
  /* The test clone owns remapped bytecode footer PCs. Keep the allocator view
  ** bound to that same source prototype and its private footer map. */
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  constant = tostring_guard_constant(f.owner);
  assert(constant && semantic_fixture(&f) && postra_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.ir[constant+LJ_GC64].gcr, obj2gco(wrong));
  assert(!semantic_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.postra[constant+LJ_GC64].gcr, obj2gco(wrong));
  assert(!postra_fixture(&f));
  arm64_fixture_free(&f);
}

static void test_wrong_cell_source(lua_State *L, const char *clone_name,
                                  TraceNo no)
{
  Arm64TraceFixture f;
  GCproto *pt;
  MSize p, callpos = 0;
  BCIns call = 0, set;
  BCReg replacement;
  if (no == 1)
    arm64_fixture_init(&f, L, clone_name);
  else
    arm64_fixture_init_trace(&f, L, clone_name, no);
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  assert(semantic_fixture(&f) && postra_fixture(&f));
  pt = f.J->pt;
  for (p = 0; p < pt->sizebc; p++) {
    BCIns ins = proto_bc(pt)[p];
    if (bc_op(ins) == BC_CALL && bc_b(ins) == 2u && bc_c(ins) == 2u) {
      call = ins;
      callpos = p;
    }
  }
  assert(call != 0 && callpos+1u < pt->sizebc);
  set = proto_bc(pt)[callpos+1u];
  assert(bc_op(set) == BC_CSET && bc_c(set) == bc_a(call));
  replacement = (BCReg)(bc_c(set)+1u);
  if (replacement >= pt->framesize) replacement = 0;
  assert(replacement != bc_a(call));
  proto_bc(pt)[callpos+1u] = BCINS_ABC(BC_CSET, bc_a(set), 0, replacement);
  arm64_fixture_reset(&f);
  assert(!semantic_fixture(&f));
  arm64_fixture_reset(&f);
  assert(!postra_fixture(&f));
  arm64_fixture_free(&f);
}

static TraceNo check_root(lua_State *L, const char *fnname, BCOp expected)
{
  GCfunc *fn;
  GCproto *pt;
  GCtrace *T;
  TraceNo no;
  lua_getglobal(L, fnname);
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  fn = funcV(L->top-1);
  pt = funcproto(fn);
  no = proto_trace_acq(pt);
  lua_pop(L, 1);
  T = traceref_safe(L2J(L), no);
  assert(no != 0 && T != NULL && trace_runnable_acq(T, no));
  assert(trace_startpt_acq(T) == pt && T->root == 0);
  assert(bc_op(trace_startins_acq(T)) == expected);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) != 0);
  return no;
}

static void positive_form(lua_State *L, const char *fnname, BCOp op)
{
  const char *out;
  size_t outlen;
  TraceNo no;
  GCtrace *T;
  MCode *code;
  MSize codesz;
  unsigned char *saved;
  unsigned entries;
  lua_Integer side_effects;
  int kind;
  lj_trace_test_reset_exit_stats();
  entries = lj_trace_test_root_entry_successes();
  call_loop(L, fnname, "tostring_builtin", 0, &out, &outlen);
  assert(outlen == 3 && memcmp(out, "a\0b", 3) == 0);
  assert(lj_trace_test_root_entry_successes() > entries);
  no = check_root(L, fnname, op);
  test_wrong_builtin_constant(L,
      op == BC_LOOP ? "tostring_while_clone" : "tostring_for_clone", no);
  test_wrong_cell_source(L,
      op == BC_LOOP ? "tostring_while_clone" : "tostring_for_clone", no);
  T = traceref_safe(L2J(L), no);
  code = trace_mcode_acq(T);
  codesz = trace_szmcode_acq(T);
  saved = (unsigned char *)malloc(codesz);
  assert(saved != NULL);
  memcpy(saved, code, codesz);

  for (kind = 0; kind < 3; kind++) {
    entries = lj_trace_test_root_entry_successes();
    call_loop(L, fnname, "tostring_builtin", kind, &out, &outlen);
    assert(lj_trace_test_root_entry_successes() > entries);
    if (kind == 0) assert(outlen == 3 && memcmp(out, "a\0b", 3) == 0);
    if (kind == 1) assert(outlen == 2 && memcmp(out, "\200x", 2) == 0);
    if (kind == 2) assert(outlen == 0);
  }

  lua_gc(L, LUA_GCCOLLECT, 0);
  entries = lj_trace_test_root_entry_successes();
  call_loop(L, fnname, "tostring_builtin", 1, &out, &outlen);
  assert(lj_trace_test_root_entry_successes() > entries && outlen == 2 &&
         memcmp(out, "\200x", 2) == 0);
  assert(trace_runnable_acq(T, no) && trace_mcode_acq(T) == code &&
         trace_szmcode_acq(T) == codesz && memcmp(saved, code, codesz) == 0);

  /* The exact string guard and fast-function guard must exit to interpreter
  ** behavior for other types and a replacement callable. */
  side_effects = global_integer(L, "tostring_side_effects");
  for (kind = 3; kind <= 5; kind++) {
    lj_trace_test_reset_exit_stats();
    entries = lj_trace_test_root_entry_successes();
    call_loop(L, fnname, "tostring_builtin", kind, &out, &outlen);
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > 0 &&
           lj_trace_test_first_exit_parent() == no);
    if (kind == 3) assert(strcmp(out, "2.75") == 0);
    if (kind == 4) assert(strcmp(out, "true") == 0);
    if (kind == 5) assert(strcmp(out, "table-tostring") == 0);
  }
  assert(global_integer(L, "tostring_side_effects") == side_effects + 40);
  side_effects += 40;
  lj_trace_test_reset_exit_stats();
  entries = lj_trace_test_root_entry_successes();
  call_loop(L, fnname, "tostring_replacement", 0, &out, &outlen);
  assert(lj_trace_test_root_entry_successes() > entries &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == no &&
         outlen == strlen("replacement:a")+2 &&
         memcmp(out, "replacement:a\0b", outlen) == 0);
  assert(global_integer(L, "tostring_side_effects") == side_effects);
  lj_trace_test_reset_exit_stats();
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop_status(L, fnname, "tostring_builtin", 6, &out, &outlen) !=
         LUA_OK);
  assert(out != NULL && strstr(out, "intentional tostring failure") != NULL);
  /* LOOP enters before the first body call and therefore exercises the
  ** string SLOAD exit. FORL enters only at the backedge, after the first
  ** call has already raised in the interpreter. */
  if (op == BC_LOOP)
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > 0 &&
           lj_trace_test_first_exit_parent() == no);
  free(saved);
  printf("tostring string LOOP/FORL: selected root %u, type/function exits, GC reuse\n",
         (unsigned)no);
}

int main(void)
{
  static const char source[] =
    "package.path='./src/?.lua;./src/?/init.lua;'..package.path\n"
    "local jit=require('jit'); assert(jit.status())\n"
    "local threading=require('threading')\n"
    "tostring_ready=threading.channel(1); tostring_stop=threading.channel(1)\n"
    "tostring_peer=assert(threading.spawn(function(ready,stop) "
    "assert(ready:send(true,5)); assert(select(2,stop:recv(120))); return true "
    "end,tostring_ready,tostring_stop))\n"
    "assert(select(2,tostring_ready:recv(5)))\n"
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8')\n"
    "tostring_builtin=tostring; tostring_side_effects=0\n"
    "tostring_table=setmetatable({}, {__tostring=function() "
    "tostring_side_effects=tostring_side_effects+1; return 'table-tostring' end})\n"
    "tostring_error_table=setmetatable({}, {__tostring=function() "
    "error('intentional tostring failure') end})\n"
    "tostring_replacement=function(v) return 'replacement:'..tostring(v) end\n"
    "tostring_while=function(to,x,n) local i,r=0,nil; "
    "while i<n do r=to(x); i=i+1 end; return r end\n"
    "tostring_while_clone=function(to,x,n) local i,r=0,nil; "
    "while i<n do r=to(x); i=i+1 end; return r end\n"
    "tostring_for=function(to,x,n) local r; "
    "for i=1,n do r=to(x) end; return r end\n"
    "tostring_for_clone=function(to,x,n) local r; "
    "for i=1,n do r=to(x) end; return r end\n";
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_chunk(L, source);
  positive_form(L, "tostring_while", BC_LOOP);
  run_chunk(L, "jit.flush()");
  positive_form(L, "tostring_for", BC_FORL);
  run_chunk(L, "assert(tostring_stop:send(true,5)); "
               "local joined,result=tostring_peer:join(5); assert(joined and result)");
  lua_close(L);
  puts("t-arm64-jit-tostring-admission OK");
  return 0;
}
