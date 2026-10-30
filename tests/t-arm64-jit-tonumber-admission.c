/* Native and source-identity checks for numeric-input tonumber identity. */
#include <assert.h>
#include <math.h>
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
#error "tonumber admission requires experimental native ARM64"
#endif

extern uint32_t lj_trace_test_root_entry_successes(void);
extern void lj_trace_test_reset_exit_stats(void);
extern uint32_t lj_trace_test_exit_calls(void);
extern TraceNo lj_trace_test_first_exit_parent(void);

static void run_chunk(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    fprintf(stderr, "tonumber fixture setup failed: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static void push_value(lua_State *L, int kind)
{
  switch (kind) {
  case 0: lua_pushinteger(L, 123); break;
  case 1: lua_pushnumber(L, 123.25); break;
  case 2: lua_pushnumber(L, NAN); break;
  case 3: lua_pushnumber(L, -0.0); break;
  case 4: lua_pushnumber(L, INFINITY); break;
  case 5: lua_pushnumber(L, -INFINITY); break;
  case 6: lua_pushliteral(L, "123.5"); break;
  case 7: lua_pushliteral(L, "not a number"); break;
  case 8: lua_pushboolean(L, 1); break;
  case 9: lua_pushnil(L); break;
  default: abort();
  }
}

static int call_loop(lua_State *L, const char *fnname,
                     const char *callee, int kind, lua_Number *out)
{
  int status;
  lua_getglobal(L, fnname);
  lua_getglobal(L, callee);
  push_value(L, kind);
  lua_pushinteger(L, 40);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "%s failed: %s\n", fnname, lua_tostring(L, -1));
    abort();
  }
  status = lua_type(L, -1);
  if (status == LUA_TNUMBER) *out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return status;
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

static IRRef tonumber_guard_constant(const GCtrace *T)
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
        lj_func_ffid_acq(gco2func(ir_kgc_acq(&T->ir[constant]))) == FF_tonumber)
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
  constant = tonumber_guard_constant(f.owner);
  assert(constant && semantic_fixture(&f) && postra_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.ir[constant+LJ_GC64].gcr, obj2gco(wrong));
  assert(!semantic_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.postra[constant+LJ_GC64].gcr, obj2gco(wrong));
  assert(!postra_fixture(&f));
  arm64_fixture_free(&f);
}

static void test_identity_builtin_constant(lua_State *L, const char *name,
                                           const char *clone_name, TraceNo no)
{
  Arm64TraceFixture f;
  IRRef constant;
  GCfunc *identity;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  identity = funcV(L->top-1);
  lua_pop(L, 1);
  if (no == 1) arm64_fixture_init(&f, L, clone_name);
  else arm64_fixture_init_trace(&f, L, clone_name, no);
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  f.view.owner_private = 1;
  constant = tonumber_guard_constant(f.owner);
  assert(constant && semantic_fixture(&f) && postra_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.ir[constant+LJ_GC64].gcr, obj2gco(identity));
  assert(semantic_fixture(&f));
  arm64_fixture_reset(&f);
  setgcref(f.postra[constant+LJ_GC64].gcr, obj2gco(identity));
  assert(postra_fixture(&f));
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
  } else if (mutation == 4u) {
    assert(callpos > 0u);
    mutatepos = callpos-1u;
    saved = proto_bc(pt)[mutatepos];
    proto_bc(pt)[mutatepos] = BCINS_ABC(BC_ISLT, 0, 0, 0);
  } else {
    mutatepos = callpos;
    saved = proto_bc(pt)[mutatepos];
    if (mutation == 5u)
      proto_bc(pt)[mutatepos] = BCINS_ABC(BC_CALL, bc_a(call), 3, bc_c(call));
    else
      proto_bc(pt)[mutatepos] = BCINS_ABC(BC_CALL, bc_a(call), bc_b(call), 3);
  }
  arm64_fixture_reset(&f);
  assert(!semantic_fixture(&f));
  arm64_fixture_reset(&f);
  assert(!postra_fixture(&f));
  proto_bc(pt)[mutatepos] = saved;
  arm64_fixture_free(&f);
}

static void test_bad_terminal_footer_and_layout(lua_State *L,
                                                const char *clone_name,
                                                TraceNo no)
{
  Arm64TraceFixture f;
  GCproto *pt;
  SnapShot *terminal;
  MSize footeridx;
  uint64_t semantic_footer, postra_footer;
  uint8_t framesize;
  if (no == 1) arm64_fixture_init(&f, L, clone_name);
  else arm64_fixture_init_trace(&f, L, clone_name, no);
  memcpy(f.postmap, f.map, trace_nsnapmap_acq(f.owner) * sizeof(SnapEntry));
  f.view.proto = f.J->pt;
  f.view.proto_bc = proto_bc(f.J->pt);
  f.view.startpc = f.J->startpc;
  f.view.owner_private = 1;
  pt = f.J->pt;
  assert(semantic_fixture(&f) && postra_fixture(&f));
  terminal = &f.snaps[f.owner->nsnap-1u];
  footeridx = terminal->mapofs+terminal->nent;
  memcpy(&semantic_footer, &f.map[footeridx], sizeof(semantic_footer));
  memcpy(&postra_footer, &f.postmap[footeridx], sizeof(postra_footer));
  f.map[footeridx] = (SnapEntry)(f.map[footeridx] | 1u);
  f.postmap[footeridx] = (SnapEntry)(f.postmap[footeridx] | 1u);
  assert(!semantic_fixture(&f));
  assert(!postra_fixture(&f));
  memcpy(&f.map[footeridx], &semantic_footer, sizeof(semantic_footer));
  memcpy(&f.postmap[footeridx], &postra_footer, sizeof(postra_footer));
  {
    uint64_t badpc = ((uint64_t)(uintptr_t)(proto_bc(pt)+pt->sizebc) << 8) |
                     (uint8_t)semantic_footer;
    memcpy(&f.map[footeridx], &badpc, sizeof(badpc));
    memcpy(&f.postmap[footeridx], &badpc, sizeof(badpc));
    assert(!semantic_fixture(&f));
    assert(!postra_fixture(&f));
    memcpy(&f.map[footeridx], &semantic_footer, sizeof(semantic_footer));
    memcpy(&f.postmap[footeridx], &postra_footer, sizeof(postra_footer));
  }
  framesize = pt->framesize;
  assert(framesize > 1u+LJ_FR2);
  pt->framesize = (uint8_t)(1u+LJ_FR2);
  assert(!semantic_fixture(&f));
  assert(!postra_fixture(&f));
  pt->framesize = framesize;
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
  constant = tonumber_guard_constant(T);
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

static void positive_form(lua_State *L, const char *fnname, BCOp op,
                          int positive_kind)
{
  static const char *wrong_builtins[] = {
    "tonumber_bad_rawset", "tonumber_bad_tostring",
    "tonumber_bad_bnot", "tonumber_bad_bswap",
    "tonumber_bad_lshift", "tonumber_bad_rshift", "tonumber_bad_arshift",
    "tonumber_bad_rol", "tonumber_bad_ror",
    "tonumber_bad_sqrt",
    "tonumber_bad_abs", "tonumber_bad_log", "tonumber_bad_log10",
    "tonumber_bad_exp", "tonumber_bad_sin", "tonumber_bad_cos",
    "tonumber_bad_tan", "tonumber_bad_asin", "tonumber_bad_acos",
    "tonumber_bad_atan", "tonumber_bad_sinh", "tonumber_bad_cosh",
    "tonumber_bad_tanh", "tonumber_bad_atan2", "tonumber_bad_pow",
    "tonumber_bad_ldexp", "tonumber_bad_byte", "tonumber_bad_sub",
    "tonumber_replacement"
  };
  static const char *identity_builtins[] = {
    "tonumber_bad_min", "tonumber_bad_max"
  };
  static const char *int_identity_builtins[] = {
    "tonumber_bad_floor", "tonumber_bad_ceil", "tonumber_bad_tobit",
    "tonumber_bad_band", "tonumber_bad_bor", "tonumber_bad_bxor"
  };
  TraceNo no;
  GCtrace *T;
  MCode *code;
  MSize codesz;
  unsigned char *saved;
  unsigned entries;
  lua_Number result;
  int kind;
  lj_trace_test_reset_exit_stats();
  entries = lj_trace_test_root_entry_successes();
  call_loop(L, fnname, "tonumber_builtin", positive_kind, &result);
  assert(result == (positive_kind == 0 ? 123 : 123.25));
  assert(lj_trace_test_root_entry_successes() > entries);
  no = check_root(L, fnname, op);
  {
    const char *clone = op == BC_LOOP ? "tonumber_while_clone" :
                                        "tonumber_for_clone";
    unsigned i;
    for (i = 0; i < sizeof(wrong_builtins)/sizeof(wrong_builtins[0]); i++)
      test_wrong_builtin_constant(L, wrong_builtins[i], clone, no);
    for (i = 0; i < sizeof(identity_builtins)/sizeof(identity_builtins[0]); i++)
      test_identity_builtin_constant(L, identity_builtins[i], clone, no);
    if (positive_kind == 0) {
      for (i = 0; i < sizeof(int_identity_builtins)/sizeof(int_identity_builtins[0]); i++)
        test_identity_builtin_constant(L, int_identity_builtins[i], clone, no);
    } else {
      for (i = 0; i < sizeof(int_identity_builtins)/sizeof(int_identity_builtins[0]); i++)
        test_wrong_builtin_constant(L, int_identity_builtins[i], clone, no);
    }
  }
  {
    unsigned mutation;
    const char *clone = op == BC_LOOP ? "tonumber_while_clone" :
                                        "tonumber_for_clone";
    for (mutation = 0; mutation < 7u; mutation++)
      test_wrong_cell_source(L, clone, no, mutation);
    test_wrong_terminal_cell_snapshot(L, clone, no);
    test_bad_terminal_footer_and_layout(L, clone, no);
  }
  T = traceref_safe(L2J(L), no);
  code = trace_mcode_acq(T);
  codesz = trace_szmcode_acq(T);
  saved = (unsigned char *)malloc(codesz);
  assert(saved != NULL);
  memcpy(saved, code, codesz);

  for (kind = 2; kind <= 5; kind++) {
    entries = lj_trace_test_root_entry_successes();
    assert(call_loop(L, fnname, "tonumber_builtin", kind, &result) == LUA_TNUMBER);
    assert(lj_trace_test_root_entry_successes() > entries);
    if (kind == 2) assert(isnan(result));
    if (kind == 3) assert(result == 0 && signbit(result));
    if (kind == 4) assert(isinf(result) && result > 0);
    if (kind == 5) assert(isinf(result) && result < 0);
  }

  lua_gc(L, LUA_GCCOLLECT, 0);
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, fnname, "tonumber_builtin", positive_kind,
                   &result) == LUA_TNUMBER);
  assert(lj_trace_test_root_entry_successes() > entries &&
         result == (positive_kind == 0 ? 123 : 123.25));
  assert(trace_runnable_acq(T, no) && trace_mcode_acq(T) == code &&
         trace_szmcode_acq(T) == codesz && memcmp(saved, code, codesz) == 0);

  /* Numeric and exact-function guards exit to ordinary Lua semantics. */
  for (kind = 6; kind <= 9; kind++) {
    lj_trace_test_reset_exit_stats();
    entries = lj_trace_test_root_entry_successes();
    int result_type = call_loop(L, fnname, "tonumber_builtin", kind, &result);
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > 0 &&
           lj_trace_test_first_exit_parent() == no);
    if (kind == 6) assert(result_type == LUA_TNUMBER && result == 123.5);
    if (kind >= 7) assert(result_type == LUA_TNIL);
  }
  lj_trace_test_reset_exit_stats();
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, fnname, "tonumber_replacement", 0, &result) == LUA_TNUMBER);
  assert(lj_trace_test_root_entry_successes() > entries &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == no && result == 133);
  free(saved);
  printf("tonumber numeric LOOP/FORL: selected root %u, type/function exits, GC reuse\n",
         (unsigned)no);
}

int main(void)
{
  static const char source[] =
    "package.path='./src/?.lua;./src/?/init.lua;'..package.path\n"
    "local jit=require('jit'); assert(jit.status())\n"
    "local threading=require('threading')\n"
    "local bit=require('bit')\n"
    "tonumber_ready=threading.channel(1); tonumber_stop=threading.channel(1)\n"
    "tonumber_peer=assert(threading.spawn(function(ready,stop) "
    "assert(ready:send(true,5)); assert(select(2,stop:recv(120))); return true "
    "end,tonumber_ready,tonumber_stop))\n"
    "assert(select(2,tonumber_ready:recv(5)))\n"
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8')\n"
    "tonumber_builtin=tonumber; tonumber_side_effects=0\n"
    "tonumber_bad_rawset=rawset; tonumber_bad_tostring=tostring; "
    "tonumber_bad_tobit=bit.tobit; tonumber_bad_band=bit.band; "
    "tonumber_bad_bor=bit.bor; tonumber_bad_bxor=bit.bxor; "
    "tonumber_bad_bnot=bit.bnot; tonumber_bad_bswap=bit.bswap; "
    "tonumber_bad_lshift=bit.lshift; tonumber_bad_rshift=bit.rshift; "
    "tonumber_bad_arshift=bit.arshift; tonumber_bad_rol=bit.rol; "
    "tonumber_bad_ror=bit.ror; tonumber_bad_floor=math.floor; "
    "tonumber_bad_ceil=math.ceil; tonumber_bad_sqrt=math.sqrt; "
    "tonumber_bad_abs=math.abs; tonumber_bad_min=math.min; "
    "tonumber_bad_max=math.max; tonumber_bad_log=math.log; "
    "tonumber_bad_log10=math.log10; tonumber_bad_exp=math.exp; "
    "tonumber_bad_sin=math.sin; tonumber_bad_cos=math.cos; "
    "tonumber_bad_tan=math.tan; tonumber_bad_asin=math.asin; "
    "tonumber_bad_acos=math.acos; tonumber_bad_atan=math.atan; "
    "tonumber_bad_sinh=math.sinh; tonumber_bad_cosh=math.cosh; "
    "tonumber_bad_tanh=math.tanh; tonumber_bad_atan2=math.atan2; "
    "tonumber_bad_pow=math.pow; tonumber_bad_ldexp=math.ldexp; "
    "tonumber_bad_byte=string.byte; tonumber_bad_sub=string.sub\n"
    "tonumber_replacement=function(v) tonumber_side_effects=tonumber_side_effects+1; return v+10 end\n"
    "tonumber_while=function(to,x,n) local i,r=0,x; "
    "while i<n do r=to(x); i=i+1 end; return r end\n"
    "tonumber_while_clone=function(to,x,n) local i,r=0,x; "
    "while i<n do r=to(x); i=i+1 end; return r end\n"
    "tonumber_for=function(to,x,n) local r=x; "
    "for i=1,n do r=to(x) end; return r end\n"
    "tonumber_for_clone=function(to,x,n) local r=x; "
    "for i=1,n do r=to(x) end; return r end\n";
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_chunk(L, source);
  positive_form(L, "tonumber_while", BC_LOOP, 0);
  run_chunk(L, "jit.flush()");
  positive_form(L, "tonumber_while", BC_LOOP, 1);
  run_chunk(L, "jit.flush()");
  positive_form(L, "tonumber_for", BC_FORL, 0);
  run_chunk(L, "jit.flush()");
  positive_form(L, "tonumber_for", BC_FORL, 1);
  run_chunk(L, "assert(tonumber_side_effects==160)");
  run_chunk(L,
    "assert(tonumber_while_clone(math.min,123,40)==123); "
    "assert(tonumber_for_clone(math.max,123.25,40)==123.25); "
    "local function optional(fn,v,n) local r; "
    "for i=1,n do r=fn(v,10) end; return r end; "
    "assert(optional(tonumber,'123',40)==123)");
  run_chunk(L, "assert(tonumber_stop:send(true,5)); "
               "local joined,result=tonumber_peer:join(5); assert(joined and result)");
  lua_close(L);
  puts("t-arm64-jit-tonumber-admission OK");
  return 0;
}
