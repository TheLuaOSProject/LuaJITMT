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

static void test_wrong_builtin_constant(lua_State *L, const char *wrong_name,
                                        const char *clone_name, TraceNo no)
{
  Arm64TraceFixture f;
  IRRef constant;
  GCfunc *wrong;
  lua_getglobal(L, wrong_name);
  assert(lua_isfunction(L, -1));
  wrong = funcV(L->top-1);
  lua_pop(L, 1);
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
  f.view.owner_private = 1;
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
                                  TraceNo no, unsigned mutation)
{
  Arm64TraceFixture f;
  GCproto *pt;
  MSize p, callpos = 0, edgepos = 0, mutatepos = 0;
  BCIns call = 0, set, saved;
  BCReg replacement;
  if (no == 1)
    arm64_fixture_init(&f, L, clone_name);
  else
    arm64_fixture_init_trace(&f, L, clone_name, no);
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  f.view.owner_private = 1;
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
  if (mutation == 0u) {
    replacement = (BCReg)(bc_c(set)+1u);
    if (replacement >= pt->framesize) replacement = 0;
    assert(replacement != bc_a(call));
    mutatepos = callpos+1u;
    saved = proto_bc(pt)[mutatepos];
    proto_bc(pt)[mutatepos] = BCINS_ABC(BC_CSET, bc_a(set), 0, replacement);
  } else if (mutation == 1u || mutation == 2u) {
    BCReg cell = bc_a(set);
    MSize footerpos = pt->sizebc-2u;
    assert(callpos+2u < footerpos);
    mutatepos = callpos+2u;
    saved = proto_bc(pt)[mutatepos];
    if (mutation == 1u)
      proto_bc(pt)[mutatepos] = BCINS_AD(BC_KNIL, cell, cell);
    else
      proto_bc(pt)[mutatepos] = BCINS_ABC(BC_VARG, cell, 0, 0);
  } else if (mutation == 3u) {
    for (p = 0; p < pt->sizebc; p++) {
      if (p != callpos &&
          bcmode_d(bc_op(proto_bc(pt)[p])) == BCMjump) {
        edgepos = p;
        break;
      }
    }
    assert(edgepos != 0);
    mutatepos = edgepos;
    saved = proto_bc(pt)[mutatepos];
    proto_bc(pt)[mutatepos] = BCINS_AJ(bc_op(saved), bc_a(saved),
      (int32_t)(callpos+1u)-(int32_t)(edgepos+1u));
  } else {
    assert(callpos > 0u);
    mutatepos = callpos-1u;
    saved = proto_bc(pt)[mutatepos];
    proto_bc(pt)[mutatepos] = BCINS_ABC(BC_ISLT, 0, 0, 0);
  }
  arm64_fixture_reset(&f);
  assert(!semantic_fixture(&f));
  arm64_fixture_reset(&f);
  assert(!postra_fixture(&f));
  proto_bc(pt)[mutatepos] = saved;
  arm64_fixture_free(&f);
}

static void test_wrong_terminal_cell_snapshot(lua_State *L,
                                               const char *clone_name,
                                               TraceNo no)
{
  Arm64TraceFixture f;
  GCproto *pt;
  GCtrace *T;
  IRRef fnref = 0, argref = 0;
  IRRef constant;
  BCIns call = 0, get;
  MSize callpos = 0, i, ofs, nent, cell, target_slot;
  SnapNo s, terminal;
  SnapEntry *semantic_entry = NULL, *postra_entry = NULL;
  if (no == 1)
    arm64_fixture_init(&f, L, clone_name);
  else
    arm64_fixture_init_trace(&f, L, clone_name, no);
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  f.view.owner_private = 1;
  T = f.owner;
  pt = f.J->pt;
  constant = tostring_guard_constant(T);
  assert(constant != 0);
  for (i = T->nk; i < T->nins; i++) {
    IRIns guard = ir_load_acq(&T->ir[i]);
    if (guard.o == IR_EQ && guard.t.irt == (IRT_FUNC|IRT_GUARD) &&
        guard.op2 == constant) { fnref = guard.op1; break; }
  }
  assert(fnref != 0);
  for (i = 0; i < pt->sizebc; i++) {
    if (bc_op(proto_bc(pt)[i]) == BC_CALL) {
      assert(call == 0);
      call = proto_bc(pt)[i]; callpos = i;
    }
  }
  assert(call != 0);
  terminal = (SnapNo)(T->nsnap-1u);
  get = proto_bc(pt)[pt->sizebc-2u];
  assert(bc_op(get) == BC_CGET);
  cell = bc_d(get);
  target_slot = 1u+LJ_FR2+cell;
  for (s = 0; s < T->nsnap; s++) {
    SnapShot *snap = &f.snaps[s];
    uint64_t footer;
    MSize sofs = snap->mapofs, snent = snap->nent;
    if (sofs > T->nsnapmap || snent > T->nsnapmap-sofs ||
        T->nsnapmap-sofs-snent < 1u+LJ_FR2) abort();
    memcpy(&footer, &f.map[sofs+snent], sizeof(footer));
    if ((uintptr_t)(footer >> 8) !=
        (uintptr_t)(const void *)(proto_bc(pt)+callpos)) continue;
    for (i = 0; i < snent; i++) {
      SnapEntry *entry = &f.map[sofs+i];
      if (( *entry & 0x00ff0000u) != 0) continue;
      if (snap_slot(*entry) == 1u+LJ_FR2+bc_a(call)+1u+LJ_FR2)
        argref = snap_ref(*entry);
    }
  }
  assert(argref != 0);
  ofs = f.snaps[terminal].mapofs; nent = f.snaps[terminal].nent;
  for (i = 0; i < nent; i++) {
    SnapEntry *a = &f.map[ofs+i], *b = &f.postmap[ofs+i];
    if (snap_slot(*a) == target_slot && (*a & 0x00ff0000u) == 0) {
      assert(snap_ref(*a) == argref);
      semantic_entry = a; postra_entry = b;
    }
  }
  assert(semantic_entry && postra_entry && semantic_fixture(&f) &&
         postra_fixture(&f));
  *semantic_entry = snap_setref(*semantic_entry, fnref);
  assert(!semantic_fixture(&f));
  *semantic_entry = snap_setref(*semantic_entry, argref);
  *postra_entry = snap_setref(*postra_entry, fnref);
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
  static const char *wrong_builtins[] = {
    "tostring_bad_rawset", "tostring_bad_tonumber",
    "tostring_bad_tobit", "tostring_bad_band", "tostring_bad_bor",
    "tostring_bad_bxor", "tostring_bad_bnot", "tostring_bad_bswap",
    "tostring_bad_lshift", "tostring_bad_rshift", "tostring_bad_arshift",
    "tostring_bad_rol", "tostring_bad_ror",
    "tostring_bad_floor", "tostring_bad_ceil", "tostring_bad_sqrt",
    "tostring_bad_abs", "tostring_bad_min", "tostring_bad_max",
    "tostring_bad_log", "tostring_bad_log10", "tostring_bad_exp",
    "tostring_bad_sin", "tostring_bad_cos", "tostring_bad_tan",
    "tostring_bad_asin", "tostring_bad_acos", "tostring_bad_atan",
    "tostring_bad_sinh", "tostring_bad_cosh", "tostring_bad_tanh",
    "tostring_bad_atan2", "tostring_bad_pow", "tostring_bad_ldexp",
    "tostring_bad_byte", "tostring_bad_sub", "tostring_replacement"
  };
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
  {
    const char *clone = op == BC_LOOP ? "tostring_while_clone" :
                                        "tostring_for_clone";
    unsigned i;
    for (i = 0; i < sizeof(wrong_builtins)/sizeof(wrong_builtins[0]); i++)
      test_wrong_builtin_constant(L, wrong_builtins[i], clone, no);
  }
  {
    unsigned mutation;
    const char *clone = op == BC_LOOP ? "tostring_while_clone" :
                                        "tostring_for_clone";
    for (mutation = 0; mutation < 5u; mutation++)
      test_wrong_cell_source(L, clone, no, mutation);
    test_wrong_terminal_cell_snapshot(L, clone, no);
  }
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
    "local bit=require('bit')\n"
    "tostring_ready=threading.channel(1); tostring_stop=threading.channel(1)\n"
    "tostring_peer=assert(threading.spawn(function(ready,stop) "
    "assert(ready:send(true,5)); assert(select(2,stop:recv(120))); return true "
    "end,tostring_ready,tostring_stop))\n"
    "assert(select(2,tostring_ready:recv(5)))\n"
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8')\n"
    "tostring_builtin=tostring; tostring_side_effects=0\n"
    "tostring_bad_rawset=rawset; tostring_bad_tonumber=tonumber; tostring_bad_tobit=bit.tobit; "
    "tostring_bad_band=bit.band; tostring_bad_bor=bit.bor; "
    "tostring_bad_bxor=bit.bxor; tostring_bad_bnot=bit.bnot; "
    "tostring_bad_bswap=bit.bswap; tostring_bad_lshift=bit.lshift; "
    "tostring_bad_rshift=bit.rshift; tostring_bad_arshift=bit.arshift; "
    "tostring_bad_rol=bit.rol; tostring_bad_ror=bit.ror; "
    "tostring_bad_floor=math.floor; tostring_bad_ceil=math.ceil; "
    "tostring_bad_sqrt=math.sqrt; tostring_bad_abs=math.abs; "
    "tostring_bad_min=math.min; tostring_bad_max=math.max; "
    "tostring_bad_log=math.log; tostring_bad_log10=math.log10; "
    "tostring_bad_exp=math.exp; tostring_bad_sin=math.sin; "
    "tostring_bad_cos=math.cos; tostring_bad_tan=math.tan; "
    "tostring_bad_asin=math.asin; tostring_bad_acos=math.acos; "
    "tostring_bad_atan=math.atan; tostring_bad_sinh=math.sinh; "
    "tostring_bad_cosh=math.cosh; tostring_bad_tanh=math.tanh; "
    "tostring_bad_atan2=math.atan2; tostring_bad_pow=math.pow; "
    "tostring_bad_ldexp=math.ldexp; tostring_bad_byte=string.byte; "
    "tostring_bad_sub=string.sub\n"
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
