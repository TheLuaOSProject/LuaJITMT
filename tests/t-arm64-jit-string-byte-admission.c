/* Independent semantic and post-RA admission tests for bounded byte loads. */
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
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"

extern int lj_trace_test_arm64_string_byte_range_call_slots(
    const GCproto *pt, MSize strslot, MSize firstslot, MSize lastslot);
extern int lj_trace_test_arm64_string_byte_range_index_ir(
    const GCtrace *T, IRRef index);
extern int lj_trace_test_arm64_string_byte_range_guard_ir(
    const GCtrace *T, IRRef guard);
extern int lj_trace_test_arm64_string_byte_range_ir(const GCtrace *T,
    IRRef strref, IRRef str, IRRef offset, IRRef expected_index,
    IRRef expected_guard);
extern int lj_trace_test_arm64_string_byte_range_index_postra(
    const LJArm64PostRAView *v, IRRef index);
extern int lj_trace_test_arm64_string_byte_range_guard_postra(
    const LJArm64PostRAView *v, IRRef guard);
extern int lj_trace_test_arm64_string_byte_range_postra(
    const LJArm64PostRAView *v, IRRef strref, IRRef str, IRRef offset,
    IRRef expected_index, IRRef expected_guard);

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

static SnapEntry snap_with_slot(SnapEntry entry, MSize slot)
{
  return (entry & 0x00ffffffu) | ((SnapEntry)slot << 24);
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

static int call_dynamic_loop_value(lua_State *L, const char *function_global,
                                  const char *value_global,
                                  lua_Number index, lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  lua_getglobal(L, value_global);
  lua_pushinteger(L, 80);
  lua_pushnumber(L, index);
  status = lua_pcall(L, 3, 1, 0);
  if (status == LUA_OK) {
    assert(lua_isnumber(L, -1));
    *out = lua_tonumber(L, -1);
  } else {
    assert(lua_isstring(L, -1));
  }
  lua_pop(L, 1);
  return status;
}

static int call_u8_snapshot_loop(lua_State *L, const char *function_global,
                                 const char *value_global, lua_Number index,
                                 lua_Integer key, lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  lua_getglobal(L, value_global);
  lua_pushinteger(L, 80);
  lua_pushnumber(L, index);
  lua_pushinteger(L, key);
  status = lua_pcall(L, 4, 1, 0);
  if (status == LUA_OK) {
    assert(lua_isnumber(L, -1));
    *out = lua_tonumber(L, -1);
  } else {
    assert(lua_isstring(L, -1));
  }
  lua_pop(L, 1);
  return status;
}

static int call_dynamic_loop(lua_State *L, const char *function_global,
                             lua_Number index, lua_Number *out)
{
  return call_dynamic_loop_value(L, function_global, "string_byte_value",
                                 index, out);
}

static int call_negative_constant_loop(lua_State *L, const char *function_global,
                                       const char *value_global,
                                       lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  lua_getglobal(L, "string_byte_builtin");
  lua_getglobal(L, value_global);
  lua_pushinteger(L, 80);
  status = lua_pcall(L, 3, 1, 0);
  if (status == LUA_OK) {
    assert(lua_isnumber(L, -1));
    *out = lua_tonumber(L, -1);
  } else {
    assert(lua_isstring(L, -1));
  }
  lua_pop(L, 1);
  return status;
}

static void test_negative_constant_variant(lua_State *L, const char *name,
                                           const char *args,
                                           const char *value_global)
{
  char source[2048], trace_name[96], oracle_name[96];
  GCtrace *owner;
  TraceNo no;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  lua_Number actual = 0.0, expected = 0.0;
  snprintf(trace_name, sizeof(trace_name), "string_byte_%s_trace", name);
  snprintf(oracle_name, sizeof(oracle_name), "string_byte_%s_oracle", name);
  snprintf(source, sizeof(source),
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8'); "
    "%s=function(byte,s,n) local i,sum=0,.5; "
    "while i<n do sum=sum+byte(s%s); i=i+1 end; return sum end; "
    "%s=function(byte,s,n) local i,sum=0,.5; "
    "while i<n do sum=sum+byte(s%s); i=i+1 end; return sum end; "
    "jit.off(%s,true)", trace_name, args, oracle_name, args, oracle_name);
  run_chunk(L, source);
  assert(call_negative_constant_loop(L, trace_name, value_global, &actual) ==
         LUA_OK);
  lua_getglobal(L, trace_name);
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  owner = traceref_safe(L2J(L), no);
  assert(no && owner && trace_runnable_acq(owner, no) &&
         bc_op(trace_startins_acq(owner)) == BC_LOOP);
  code = trace_mcode_acq(owner);
  codesize = trace_szmcode_acq(owner);
  assert(code && codesize);
  saved = (unsigned char *)malloc(codesize);
  assert(saved);
  memcpy(saved, code, codesize);
  {
    static const char *const values[] = {
      "string_byte_negative_constant_value",
      "string_byte_negative_constant_nul",
      "string_byte_negative_constant_high",
      "string_byte_negative_constant_pair"
    };
    unsigned i;
    for (i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
      unsigned entries;
      assert(call_negative_constant_loop(L, oracle_name, values[i],
                                         &expected) == LUA_OK);
      lj_trace_test_reset_exit_stats();
      entries = lj_trace_test_root_entry_successes();
      assert(call_negative_constant_loop(L, trace_name, values[i], &actual) ==
             LUA_OK && actual == expected);
      assert(lj_trace_test_root_entry_successes() > entries);
    }
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(call_negative_constant_loop(L, oracle_name, value_global,
                                     &expected) == LUA_OK);
  lj_trace_test_reset_exit_stats();
  {
    unsigned entries = lj_trace_test_root_entry_successes();
    assert(call_negative_constant_loop(L, trace_name, value_global, &actual) ==
           LUA_OK && actual == expected);
    assert(lj_trace_test_root_entry_successes() > entries);
  }
  assert(trace_runnable_acq(owner, no) && trace_mcode_acq(owner) == code &&
         trace_szmcode_acq(owner) == codesize &&
         memcmp(saved, code, codesize) == 0);
  if (strcmp(name, "negative_one") == 0) {
    assert(call_negative_constant_loop(L, oracle_name,
        "string_byte_negative_constant_short", &expected) == LUA_OK);
    lj_trace_test_reset_exit_stats();
    assert(call_negative_constant_loop(L, trace_name,
        "string_byte_negative_constant_short", &actual) == LUA_OK &&
        actual == expected);
  } else {
    assert(call_negative_constant_loop(L, oracle_name,
        "string_byte_negative_constant_short", &actual) != LUA_OK);
    lj_trace_test_reset_exit_stats();
    assert(call_negative_constant_loop(L, trace_name,
        "string_byte_negative_constant_short", &actual) != LUA_OK);
  }
  assert(call_negative_constant_loop(L, oracle_name,
      "string_byte_negative_constant_empty", &actual) != LUA_OK);
  lj_trace_test_reset_exit_stats();
  assert(call_negative_constant_loop(L, trace_name,
      "string_byte_negative_constant_empty", &actual) != LUA_OK);
  assert(lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exit_parent() == no);
  free(saved);
}

static int call_integer_index_loop(lua_State *L, const char *function_global,
                                   const char *string_global,
                                   lua_Number index, int integer,
                                   lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  lua_getglobal(L, string_global);
  lua_pushinteger(L, 80);
  if (integer) lua_pushinteger(L, (lua_Integer)index);
  else lua_pushnumber(L, index);
  status = lua_pcall(L, 3, 1, 0);
  if (status == LUA_OK) {
    assert(lua_isnumber(L, -1));
    *out = lua_tonumber(L, -1);
  } else {
    assert(lua_isstring(L, -1));
  }
  lua_pop(L, 1);
  return status;
}

static int call_start_end_loop(lua_State *L, const char *function_global,
                               const char *string_global, lua_Number first,
                               int first_integer, lua_Number last,
                               int last_integer, lua_Number *out)
{
  int status;
  lua_getglobal(L, function_global);
  lua_getglobal(L, string_global);
  lua_pushinteger(L, 40);
  if (first_integer) lua_pushinteger(L, (lua_Integer)first);
  else lua_pushnumber(L, first);
  if (last_integer) lua_pushinteger(L, (lua_Integer)last);
  else lua_pushnumber(L, last);
  status = lua_pcall(L, 4, 1, 0);
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
    if (ir[r].o == IR_KNUM || ir[r].o == IR_KGC ||
	ir[r].o == IR_KKPTR) r++;
  }
  return 0;
}

static IRIns *insert_irref(const IRIns *ir, IRRef nins, IRRef at,
                           IRIns inserted)
{
  IRIns *copy;
  IRRef ref;
  assert(at >= REF_FIRST && at < nins);
  copy = (IRIns *)calloc((size_t)nins+1u, sizeof(IRIns));
  assert(copy);
  memcpy(copy, ir, (size_t)at*sizeof(IRIns));
  copy[at] = inserted;
  memcpy(copy+at+1u, ir+at, (size_t)(nins-at)*sizeof(IRIns));
  for (ref = at+1u; ref <= nins; ref++) {
    IRMode mode = lj_ir_mode[copy[ref].o];
    if (irm_op1(mode) == IRMref && copy[ref].op1 >= at &&
        copy[ref].op1 < nins) copy[ref].op1++;
    if (irm_op2(mode) == IRMref && copy[ref].op2 >= at &&
        copy[ref].op2 < nins) copy[ref].op2++;
  }
  return copy;
}

static void test_fixed_byte_call(lua_State *L, const char *arguments,
                                 int32_t expected_offset, int32_t expected_bound)
{
  Arm64TraceFixture f;
  char source[1536];
  const char *parameter = strstr(arguments,"last") ? ",last" : "";
  GCtrace *owner;
  TraceNo no;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  IRRef strref=0, length=0, guard=0;
  lua_Number actual, expected;
  const char *values[]={"string_byte_value", expected_offset == 0 ?
                        "string_byte_short" : "string_byte_fixed_exact",
                        "string_byte_default_nul", "string_byte_default_high"};
  snprintf(source,sizeof(source),
    "jit.flush(); "
    "string_byte_default_trace=function(s,n%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s%s); i=i+1 end; return sum end; "
    "string_byte_default_clone=function(s,n%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s%s); i=i+1 end; return sum end; "
    "string_byte_default_oracle=function(s,n%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s%s); i=i+1 end; return sum end; "
    "jit.off(string_byte_default_oracle,true); "
    "string_byte_fixed_exact='ab'; "
    "string_byte_default_nul='\\0\\0a'; string_byte_default_high='\\255\\255a'",
    parameter,arguments,parameter,arguments,parameter,arguments);
  run_chunk(L,source);
  assert(call_loop(L,"string_byte_default_trace","string_byte_value",&actual)==LUA_OK);
  lua_getglobal(L,"string_byte_default_trace");
  no=proto_trace_acq(funcproto(funcV(L->top-1))); lua_pop(L,1);
  owner=traceref_safe(L2J(L),no);
  assert(no && owner && trace_runnable_acq(owner,no));
  code=trace_mcode_acq(owner); codesize=trace_szmcode_acq(owner);
  assert(code && codesize);
  saved=(unsigned char *)malloc(codesize); assert(saved);
  memcpy(saved,code,codesize);
  for (unsigned phase=0;phase<2;phase++) {
    if (phase) lua_gc(L,LUA_GCCOLLECT,0);
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
      assert(call_loop(L,"string_byte_default_oracle",values[i],&expected)==LUA_OK);
      lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
      assert(call_loop(L,"string_byte_default_trace",values[i],&actual)==LUA_OK &&
             actual==expected);
      assert(lj_trace_test_root_entry_successes()>0 &&
             lj_trace_test_exit_calls()>0 && lj_trace_test_last_exit_parent()==no);
    }
    assert(trace_runnable_acq(owner,no) && trace_mcode_acq(owner)==code &&
           trace_szmcode_acq(owner)==codesize && memcmp(saved,code,codesize)==0);
  }
  assert(call_loop(L,"string_byte_default_oracle","string_byte_empty",&expected)!=LUA_OK);
  lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
  assert(call_loop(L,"string_byte_default_trace","string_byte_empty",&actual)!=LUA_OK);
  assert(lj_trace_test_root_entry_successes()>0 &&
         lj_trace_test_exit_calls()>0 && lj_trace_test_first_exit_parent()==no);
  assert(call_loop(L,"string_byte_default_trace","string_byte_value",&actual)==LUA_OK);
  if (*parameter) {
    const lua_Number ends[] = { 1.0, 4.0, -2.0 };
    for (unsigned i=0;i<sizeof(ends)/sizeof(ends[0]);i++) {
      int expected_status=call_dynamic_loop_value(L,
          "string_byte_default_oracle","string_byte_value",ends[i],&expected);
      int actual_status;
      lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
      actual_status=call_dynamic_loop_value(L,"string_byte_default_trace",
          "string_byte_value",ends[i],&actual);
      assert(actual_status==expected_status &&
             (actual_status!=LUA_OK || actual==expected));
      assert(lj_trace_test_root_entry_successes()>0 &&
             lj_trace_test_exit_calls()>0 && lj_trace_test_first_exit_parent()==no);
    }
    assert(call_loop(L,"string_byte_default_trace","string_byte_value",&actual)==LUA_OK);
    assert(trace_runnable_acq(owner,no) && trace_mcode_acq(owner)==code &&
           trace_szmcode_acq(owner)==codesize && memcmp(saved,code,codesize)==0);
  }
  arm64_fixture_init_trace(&f,L,"string_byte_default_clone",no);
  assert(semantic(&f) && allocated(&f));
  for (IRRef r=REF_FIRST;r<f.semantic_nins;r++) {
    if (f.ir[r].o==IR_STRREF) strref=r;
    if (f.ir[r].o==IR_FLOAD && f.ir[r].op2==IRFL_STR_LEN) length=r;
  }
  assert(strref && length);
  {
    int32_t offset;
    assert(kint(f.ir,f.semantic.nk,f.ir[strref].op2,&offset) &&
           offset==expected_offset);
    for (IRRef r=length+1u;r<strref;r++) {
      int32_t bound;
      if (f.ir[r].o==IR_UGE && f.ir[r].op1==length &&
          kint(f.ir,f.semantic.nk,f.ir[r].op2,&bound) &&
          bound==expected_bound) guard=r;
    }
  }
  assert(guard);
  f.ir[strref].op2=f.ir[guard].op2;
  f.postra[strref].op2=f.postra[guard].op2;
  assert(!semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  f.ir[guard].t.irt &= (uint8_t)~IRT_GUARD;
  f.postra[guard].t.irt &= (uint8_t)~IRT_GUARD;
  assert(!semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  /* A bound at the offset itself does not prove that byte is in the string. */
  f.ir[guard].op2=f.ir[strref].op2;
  f.postra[guard].op2=f.postra[strref].op2;
  assert(!semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f); lua_pop(L,1); free(saved);
  printf("string.byte(s%s): native entry, NUL/high byte, empty exit, "
         "GC/code reuse, three independent bounds negatives\n", arguments);
}


/* Fixed multiple returns still use one independent bounds proof per byte.
** Distinct weighted values expose dropped, duplicated or permuted results. */
static int call_multiple_loop(lua_State *L, unsigned mode, lua_Number first,
                               lua_Number last,
                               const char *fn, const char *string,
                               lua_Number *out)
{
  int status;
  int first_numeric = mode == 6u || mode == 8u || mode == 10u;
  int last_numeric = mode == 2u || mode == 4u || mode == 6u ||
                     mode == 9u || mode == 10u;
  if (!mode) return call_loop(L,fn,string,out);
  lua_getglobal(L,fn);
  if (string) lua_getglobal(L,string);
  else lua_newtable(L);
  lua_pushinteger(L,80);
  if (mode>=5) {
    if (first_numeric) lua_pushnumber(L,first);
    else lua_pushinteger(L,(lua_Integer)first);
  }
  if (last_numeric) lua_pushnumber(L,last);
  else lua_pushinteger(L,(lua_Integer)last);
  status=lua_pcall(L,mode>=5 ? 4 : 3,1,0);
  if (status==LUA_OK) { assert(lua_isnumber(L,-1)); *out=lua_tonumber(L,-1); }
  lua_pop(L,1);
  return status;
}

static void test_fixed_multiple_bytes(lua_State *L, unsigned width,
                                      unsigned first, unsigned mode)
{
  Arm64TraceFixture f;
  char body[4096], source[14000], bytes[40], firstarg[20], endarg[20];
  size_t used;
  GCtrace *owner;
  TraceNo no;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  unsigned i, phase, pointers=0, loads=0, snapshot_checks=0;
  IRRef r, loadref=0, lastptr=0, widthguard=0;
  IRRef first_negative=0, last_negative=0, base_nonnegative=0;
  IRRef width_difference=0, difference_guard=0;
  IRRef range_first=0, range_last=0, negative_len=0;
  lua_Number actual=0, expected=0, weighted=0, recorded_first=first, recorded_end;
  int negative_range = mode >= 7u;
  assert(width>=2 && width<=16 && first>=1 && first<=2 && mode<=10);
  recorded_end=first+width-1u;
  if (mode==3 || mode==4) recorded_end-=(lua_Number)sizeof(bytes)+1;
  if (mode==6) recorded_first+=.9;
  if (mode==2 || mode==6) recorded_end+=.9;
  if (mode==4) recorded_end-=.9;
  if (negative_range) {
    recorded_first -= (lua_Number)sizeof(bytes)+1;
    recorded_end -= (lua_Number)sizeof(bytes)+1;
  }
  if (mode) strcpy(endarg,"last");
  else snprintf(endarg,sizeof(endarg),"%u",first+width-1u);
  if (mode>=5) strcpy(firstarg,"first");
  else snprintf(firstarg,sizeof(firstarg),"%u",first);
  for (i=0;i<sizeof(bytes);i++) bytes[i]=(char)((i*37u+128u)&255u);
  bytes[first-1u]=0;
  for (i=0;i<width;i++)
    weighted+=(lua_Number)(unsigned char)bytes[first-1u+i]*(i+1.25);
  lua_pushlstring(L,bytes,sizeof(bytes));
  lua_setglobal(L,"string_byte_multi_value");
  lua_pushlstring(L,bytes,first+width-1u);
  lua_setglobal(L,"string_byte_multi_exact");
  lua_pushlstring(L,bytes,first+width-2u);
  lua_setglobal(L,"string_byte_multi_short");
  used=(size_t)snprintf(body,sizeof(body),
    "function(s,n%s) local byte=string.byte;local sum,i=.5,0;while i<n do i=i+1;local ",
    mode>=5 ? ",first,last" : mode ? ",last" : "");
  for (i=0;i<width;i++)
    used+=(size_t)snprintf(body+used,sizeof(body)-used,"%sb%u",i ? "," : "",i);
  used+=(size_t)snprintf(body+used,sizeof(body)-used,
    "=byte(s,%s,%s);sum=sum",firstarg,endarg);
  for (i=0;i<width;i++)
    used+=(size_t)snprintf(body+used,sizeof(body)-used,"+b%u*%u.25",i,i+1u);
  used+=(size_t)snprintf(body+used,sizeof(body)-used," end;return sum end");
  assert(used<sizeof(body));
  assert(snprintf(source,sizeof(source),
    "jit.flush();string_byte_multi_trace=%s;string_byte_multi_clone=%s;"
    "string_byte_multi_oracle=%s;jit.off(string_byte_multi_oracle,true)",
    body,body,body)<(int)sizeof(source));
  run_chunk(L,source);
  assert(call_multiple_loop(L,mode,recorded_first,recorded_end,"string_byte_multi_trace","string_byte_multi_value",
                   &actual)==LUA_OK && actual==.5+80*weighted);
  lua_getglobal(L,"string_byte_multi_trace");
  no=proto_trace_acq(funcproto(funcV(L->top-1)));lua_pop(L,1);
  owner=traceref_safe(L2J(L),no);
  assert(no && owner && trace_runnable_acq(owner,no));
  code=trace_mcode_acq(owner);codesize=trace_szmcode_acq(owner);
  assert(code && codesize);
  saved=(unsigned char *)malloc(codesize);assert(saved);
  memcpy(saved,code,codesize);
  for (phase=0;phase<2;phase++) {
    const char *values[]={"string_byte_multi_value","string_byte_multi_exact",
                           "string_byte_multi_short","string_byte_empty",NULL};
    if (phase) lua_gc(L,LUA_GCCOLLECT,0);
    for (i=0;i<sizeof(values)/sizeof(values[0]);i++) {
      int want=call_multiple_loop(L,mode,recorded_first,recorded_end,"string_byte_multi_oracle",values[i],&expected);
      int got;
      lj_trace_test_root_entry_reset();lj_trace_test_reset_exit_stats();
      got=call_multiple_loop(L,mode,recorded_first,recorded_end,"string_byte_multi_trace",values[i],&actual);
      assert(got==want && (got!=LUA_OK || actual==expected));
      assert(lj_trace_test_root_entry_successes()>0 &&
             lj_trace_test_exit_calls()>0 &&
             lj_trace_test_first_exit_parent()==no);
    }
    if (mode>=5) {
      const struct { lua_Number first, last; unsigned mode; } bounds[]={
        {first+1,first+width-1u,5}, {first,first+width,5},
        {0,first+width-1u,5}, {-1,first+width-1u,5}, {first,-1,5},
        {INT32_MIN,first+width-1u,5}, {INT32_MAX,first+width-1u,5},
        {first+.9,first+width-1u+.9,6},
        {NAN,first+width-1u,6}, {first,NAN,6},
        {INFINITY,first+width-1u,6}, {-INFINITY,first+width-1u,6},
        {first,INFINITY,6}, {first,-INFINITY,6},
        {1.0e300,first+width-1u,6}, {-1.0e300,first+width-1u,6}
      };
      for (i=0;i<sizeof(bounds)/sizeof(bounds[0]);i++) {
        int want=call_multiple_loop(L,bounds[i].mode,bounds[i].first,
          bounds[i].last,"string_byte_multi_oracle","string_byte_multi_value",
          &expected), got;
        lj_trace_test_root_entry_reset();lj_trace_test_reset_exit_stats();
        got=call_multiple_loop(L,bounds[i].mode,bounds[i].first,bounds[i].last,
          "string_byte_multi_trace","string_byte_multi_value",&actual);
        assert(got==want && (got!=LUA_OK || actual==expected));
        assert(lj_trace_test_root_entry_successes()>0 &&
               lj_trace_test_exit_calls()>0 &&
               lj_trace_test_first_exit_parent()==no);
      }
      if (negative_range) {
        const struct { lua_Number first, last; } negative_bounds[]={
          {recorded_first+1,recorded_end},
          {recorded_first,recorded_end+1},
          {0,recorded_end}, {recorded_first,0},
          {1,recorded_end}, {recorded_first,1},
          {-1,-1}, {-40,-39}
        };
        for (i=0;i<sizeof(negative_bounds)/sizeof(negative_bounds[0]);i++) {
          int want=call_multiple_loop(L,mode,negative_bounds[i].first,
            negative_bounds[i].last,"string_byte_multi_oracle",
            "string_byte_multi_value",&expected), got;
          lj_trace_test_root_entry_reset();lj_trace_test_reset_exit_stats();
          got=call_multiple_loop(L,mode,negative_bounds[i].first,
            negative_bounds[i].last,"string_byte_multi_trace",
            "string_byte_multi_value",&actual);
          assert(got==want && (got!=LUA_OK || actual==expected));
          assert(lj_trace_test_root_entry_successes()>0 &&
                 lj_trace_test_exit_calls()>0 &&
                 lj_trace_test_first_exit_parent()==no);
        }
        if (mode==8u || mode==10u) {
          int want=call_multiple_loop(L,mode,NAN,recorded_end,
            "string_byte_multi_oracle","string_byte_multi_value",&expected);
          lj_trace_test_root_entry_reset();lj_trace_test_reset_exit_stats();
          int got=call_multiple_loop(L,mode,NAN,recorded_end,
            "string_byte_multi_trace","string_byte_multi_value",&actual);
          assert(got==want && (got!=LUA_OK || actual==expected));
          assert(lj_trace_test_root_entry_successes()>0 &&
                 lj_trace_test_exit_calls()>0 &&
                 lj_trace_test_first_exit_parent()==no);
        }
        if (mode==9u || mode==10u) {
          int want=call_multiple_loop(L,mode,recorded_first,NAN,
            "string_byte_multi_oracle","string_byte_multi_value",&expected);
          lj_trace_test_root_entry_reset();lj_trace_test_reset_exit_stats();
          int got=call_multiple_loop(L,mode,recorded_first,NAN,
            "string_byte_multi_trace","string_byte_multi_value",&actual);
          assert(got==want && (got!=LUA_OK || actual==expected));
          assert(lj_trace_test_root_entry_successes()>0 &&
                 lj_trace_test_exit_calls()>0 &&
                 lj_trace_test_first_exit_parent()==no);
        }
      }
    }
    assert(call_multiple_loop(L,mode,recorded_first,recorded_end,"string_byte_multi_trace","string_byte_multi_value",
                     &actual)==LUA_OK && actual==.5+80*weighted);
    assert(trace_runnable_acq(owner,no) && trace_mcode_acq(owner)==code &&
           trace_szmcode_acq(owner)==codesize && memcmp(saved,code,codesize)==0);
  }
  arm64_fixture_init_trace(&f,L,"string_byte_multi_clone",no);
  if (mode>=5) f.view.proto=trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  for (r=REF_FIRST;r<f.semantic_nins;r++) {
    int32_t k;
    if (mode && f.ir[r].o==IR_EQ && f.ir[r].t.irt==(IRT_INT|IRT_GUARD) &&
        kint(f.ir,f.semantic.nk,f.ir[r].op2,&k) && k==(int32_t)width)
      widthguard=r;
    if (f.ir[r].o==IR_STRREF) {
      IRRef q, bound=0;
      int32_t offset;
      if (mode>=5) {
        IRRef base, at=f.ir[r].op2;
        assert(widthguard && f.ir[f.ir[widthguard].op1].o==IR_SUBOV);
        base=f.ir[f.ir[widthguard].op1].op2;
        if (negative_range && !range_first) {
          IRRef lenref, endref=f.ir[f.ir[widthguard].op1].op1;
          IRRef inner, sum, last, first;
          assert(base>=REF_FIRST && f.ir[base].o==IR_ADD &&
                 f.ir[base].t.irt==IRT_INT);
          if (f.ir[base].op1>=REF_FIRST &&
              f.ir[f.ir[base].op1].o==IR_FLOAD) {
            lenref=f.ir[base].op1; first=f.ir[base].op2;
          } else {
            lenref=f.ir[base].op2; first=f.ir[base].op1;
          }
          assert(lenref>=REF_FIRST && f.ir[lenref].o==IR_FLOAD &&
                 f.ir[lenref].op1==f.ir[r].op1 &&
                 f.ir[lenref].op2==IRFL_STR_LEN);
          negative_len=lenref;
          assert(endref>=REF_FIRST && f.ir[endref].o==IR_ADD &&
                 kint(f.ir,f.semantic.nk,f.ir[endref].op2,&k) && k==1);
          inner=f.ir[endref].op1;
          assert(inner>=REF_FIRST && f.ir[inner].o==IR_ADD);
          sum=inner;
          if (f.ir[sum].op1==lenref) last=f.ir[sum].op2;
          else { assert(f.ir[sum].op2==lenref); last=f.ir[sum].op1; }
          range_first=first; range_last=last;
          for (q=REF_FIRST;q<base;q++)
            if (f.ir[q].o==IR_LT && f.ir[q].t.irt==(IRT_INT|IRT_GUARD) &&
                f.ir[q].op1==first && kint(f.ir,f.semantic.nk,f.ir[q].op2,&k) &&
                k==0) first_negative=q;
          for (q=REF_FIRST;q<f.ir[endref].op1;q++)
            if (f.ir[q].o==IR_LT && f.ir[q].t.irt==(IRT_INT|IRT_GUARD) &&
                f.ir[q].op1==last && kint(f.ir,f.semantic.nk,f.ir[q].op2,&k) &&
                k==0) last_negative=q;
          for (q=base+1u;q<r;q++) {
            if (f.ir[q].o==IR_GE && f.ir[q].t.irt==(IRT_INT|IRT_GUARD) &&
                f.ir[q].op1==base && kint(f.ir,f.semantic.nk,f.ir[q].op2,&k) &&
                k==0) base_nonnegative=q;
            if (f.ir[q].o==IR_SUBOV && f.ir[q].t.irt==(IRT_INT|IRT_GUARD) &&
                f.ir[q].op1==endref && f.ir[q].op2==base)
              difference_guard=q;
          }
          width_difference=f.ir[widthguard].op1;
          assert(first_negative && last_negative && base_nonnegative &&
                 width_difference && difference_guard &&
                 lj_trace_test_arm64_string_byte_range_ir(&f.semantic,r,
                   f.ir[r].op1,at,range_first,0) &&
                 lj_trace_test_arm64_string_byte_range_postra(&f.view,r,
                   f.postra[r].op1,at,range_first,0));
        }
        if (at==base) offset=0;
        else {
          assert(at>=REF_FIRST && at<r && f.ir[at].o==IR_ADD &&
                 f.ir[at].op1==base &&
                 kint(f.ir,f.semantic.nk,f.ir[at].op2,&offset));
        }
        assert(offset>=0 && offset<(int32_t)width);
        if (negative_range)
          assert(lj_trace_test_arm64_string_byte_range_ir(&f.semantic,r,
                     f.ir[r].op1,at,range_first,0) &&
                 lj_trace_test_arm64_string_byte_range_postra(&f.view,r,
                     f.postra[r].op1,at,range_first,0));
      } else {
        assert(kint(f.ir,f.semantic.nk,f.ir[r].op2,&offset) &&
               offset>=(int32_t)first-1 && offset<(int32_t)(first+width-1u));
      }
      for (q=REF_FIRST;q<r;q++) {
        IRRef length=f.ir[q].o==IR_UGE ? f.ir[q].op1 :
                     f.ir[q].o==IR_ULE ? f.ir[q].op2 : 0;
        if (length>=REF_FIRST && f.ir[length].o==IR_FLOAD &&
            f.ir[length].op1==f.ir[r].op1)
          bound=f.ir[q].o==IR_UGE ? f.ir[q].op2 : f.ir[q].op1;
      }
      assert(bound || mode>=3);
      if (offset==(int32_t)(mode>=5 ? width-1u : first+width-2u)) lastptr=r;
      if (!bound) bound=REF_TRUE;
      f.ir[r].op2=bound;f.postra[r].op2=bound;
      assert(!semantic(&f) && !allocated(&f));
      arm64_fixture_reset(&f);
      pointers++;
    } else if (f.ir[r].o==IR_XLOAD) {
      assert(f.ir[r].t.irt==IRT_U8 && f.ir[r].op2==IRXLOAD_READONLY);
      f.ir[r].op2=0;f.postra[r].op2=0;
      assert(!semantic(&f) && !allocated(&f));
      arm64_fixture_reset(&f);
      if (!loadref) loadref=r;
      loads++;
    }
  }
  assert(pointers>=width && loads==pointers && loadref && lastptr);
  if (mode) {
    IRRef oldnk=f.semantic.nk, newref=oldnk-1u;
    assert(widthguard && oldnk>1u && f.view.nk==oldnk);
    f.ir[widthguard].o=IR_NE;f.postra[widthguard].o=IR_NE;
    assert(!semantic(&f) && !allocated(&f));
    arm64_fixture_reset(&f);
    /* A separate constant avoids mutating an aliased width/base literal.
    ** Admit the valid last byte, then refuse exactly one byte past it. */
    f.semantic.nk=f.view.nk=newref;
    memset(&f.ir[newref],0,sizeof(IRIns));
    f.ir[newref].o=IR_KINT;f.ir[newref].t.irt=IRT_INT;
    f.ir[newref].i=(int32_t)(mode>=5 ? width-1u : first+width-2u);
    f.postra[newref]=f.ir[newref];
    if (mode>=5) {
      IRRef add=f.ir[lastptr].op2;
      assert(f.ir[add].o==IR_ADD);
      f.ir[add].op2=f.postra[add].op2=newref;
    } else {
      f.ir[lastptr].op2=f.postra[lastptr].op2=newref;
    }
    assert(semantic(&f) && allocated(&f));
    f.ir[newref].i++;f.postra[newref].i++;
    assert(!semantic(&f) && !allocated(&f));
    f.semantic.nk=f.view.nk=oldnk;
    arm64_fixture_reset(&f);
  }
  if (negative_range) {
    assert(first_negative && last_negative && base_nonnegative &&
           width_difference && difference_guard && range_first && range_last &&
           negative_len);
#define REJECT_NEG_MULTI(change) do { \
    arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
    change; assert(!semantic(&f) && !allocated(&f)); \
  } while (0)
    REJECT_NEG_MULTI(f.ir[first_negative].o=IR_GE;
                     f.postra[first_negative].o=IR_GE);
    REJECT_NEG_MULTI(f.ir[last_negative].op1=range_first;
                     f.postra[last_negative].op1=range_first);
    REJECT_NEG_MULTI(f.ir[width_difference].op2=range_first;
                     f.postra[width_difference].op2=range_first);
    assert(difference_guard+1u<f.semantic.nins);
    REJECT_NEG_MULTI(f.ir[difference_guard].op1=difference_guard+1u;
                     f.postra[difference_guard].op1=difference_guard+1u);
    REJECT_NEG_MULTI(f.ir[base_nonnegative].o=IR_GT;
                     f.postra[base_nonnegative].o=IR_GT);
    REJECT_NEG_MULTI(f.ir[negative_len].op1=range_first;
                     f.postra[negative_len].op1=range_first);
    REJECT_NEG_MULTI(f.ir[range_first].op1=f.ir[range_last].op1;
                     f.postra[range_first].op1=f.postra[range_last].op1);
#undef REJECT_NEG_MULTI
    arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  }
  if (mode>=5) {
    IRRef add=f.ir[lastptr].op2, base=f.ir[add].op1;
    assert(f.ir[add].o==IR_ADD && lastptr==add+1u &&
           !ra_hasreg(f.postra[add].r) && !ra_hasspill(f.postra[add].s));
    /* The absent ADD result is valid only for the adjacent fused STRREF;
    ** allocating that pointer cannot certify the same missing result. */
    f.postra[lastptr].r=RID_X19;
    assert(semantic(&f) && !allocated(&f));
    arm64_fixture_reset(&f);
    /* Its checked base must have an actual register or spill location. */
    f.postra[base].r=RID_INIT;f.postra[base].s=SPS_NONE;
    assert(semantic(&f) && !allocated(&f));
    arm64_fixture_reset(&f);
    assert(semantic(&f) && allocated(&f));
  }
  /* Raw U8 values are still excluded from Lua snapshot restoration. */
  for (SnapNo sn=0;sn<f.view.nsnap;sn++) {
    SnapShot *snap=&f.snaps[sn];
    if (snap->ref>loadref && snap->nent>2u) {
      MSize at=snap->mapofs+snap->nent-1u;
      SnapEntry old=f.map[at], postold=f.postmap[at];
      f.map[at]=snap_setref(old,loadref);
      f.postmap[at]=snap_setref(postold,loadref);
      assert(!semantic(&f) && !allocated(&f));
      f.map[at]=old;f.postmap[at]=postold;
      snapshot_checks++;
      break;
    }
  }
  assert(snapshot_checks==1u);
  arm64_fixture_reset(&f);assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);lua_pop(L,1);free(saved);
  printf("string.byte mode %u, %u results from %u: native weighted bytes, short/type exits, GC/code reuse, %u per-byte proof mutations\n",
         mode,width,first,pointers+loads);
}

static int call_range_loop(lua_State *L, const char *fn, const char *string,
                           lua_Number last, int numeric, lua_Number *value)
{
  int status;
  lua_getglobal(L,fn); lua_getglobal(L,string); lua_pushinteger(L,80);
  if (numeric) lua_pushnumber(L,last);
  else lua_pushinteger(L,(lua_Integer)last);
  status=lua_pcall(L,3,1,0);
  if (status==LUA_OK) { assert(lua_isnumber(L,-1)); *value=lua_tonumber(L,-1); }
  lua_pop(L,1);
  return status;
}

static void test_dynamic_range_byte(lua_State *L, lua_Number recorded_end,
                                    int numeric_end)
{
  Arm64TraceFixture f;
  TraceNo no;
  GCtrace *owner;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  IRRef length=0, upper=0, end=0, difference=0, width=0, pointer=0;
  IRRef index=0, sum=0;
  int negative_end=recorded_end<0;
  lua_Number actual=0, expected=0;
  static const struct { const char *string; lua_Number last; int numeric; } cases[]={
    {"string_byte_value",2,0}, {"string_byte_value",4,0},
    {"string_byte_value",1,0}, {"string_byte_value",0,0},
    {"string_byte_value",-2,0}, {"string_byte_value",INT32_MIN,0},
    {"string_byte_value",INT32_MAX,0}, {"string_byte_value",4.9,1},
    {"string_byte_value",-2.9,1}, {"string_byte_value",-1.9,1},
    {"string_byte_value",NAN,1}, {"string_byte_value",INFINITY,1},
    {"string_byte_value",-INFINITY,1}, {"string_byte_short",4,0},
    {"string_byte_value",2147483648.0,1},
    {"string_byte_value",-2147483649.0,1},
    {"string_byte_value",1.0e300,1}, {"string_byte_value",-1.0e300,1},
    {"string_byte_empty",4,0}, {"string_byte_default_nul",2,0},
    {"string_byte_default_high",2,0}, {"string_byte_long",65536,0}
  };
  run_chunk(L,
    "jit.flush(); "
    "string_byte_range_value='byte-test'; "
    "string_byte_range_trace=function(s,n,last) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s,2,last); i=i+1 end; return sum end; "
    "string_byte_range_clone=function(s,n,last) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s,2,last); i=i+1 end; return sum end; "
    "string_byte_range_oracle=function(s,n,last) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+byte(s,2,last); i=i+1 end; return sum end; "
    "jit.off(string_byte_range_oracle,true)");
  assert(call_range_loop(L,"string_byte_range_trace","string_byte_range_value",
                         recorded_end,numeric_end,&actual)==LUA_OK);
  lua_getglobal(L,"string_byte_range_trace");
  no=proto_trace_acq(funcproto(funcV(L->top-1))); lua_pop(L,1);
  owner=traceref_safe(L2J(L),no);
  assert(no && owner && trace_runnable_acq(owner,no));
  code=trace_mcode_acq(owner); codesize=trace_szmcode_acq(owner);
  assert(code && codesize); saved=(unsigned char *)malloc(codesize); assert(saved);
  memcpy(saved,code,codesize);
  for (unsigned phase=0;phase<2;phase++) {
    if (phase) lua_gc(L,LUA_GCCOLLECT,0);
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
      int expected_status=call_range_loop(L,"string_byte_range_oracle",
          cases[i].string,cases[i].last,cases[i].numeric,&expected);
      int actual_status;
      lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
      actual_status=call_range_loop(L,"string_byte_range_trace",
          cases[i].string,cases[i].last,cases[i].numeric,&actual);
      assert(actual_status==expected_status &&
             (actual_status!=LUA_OK || actual==expected));
      assert(lj_trace_test_root_entry_successes()>0 &&
             lj_trace_test_exit_calls()>0 && lj_trace_test_first_exit_parent()==no);
    }
    assert(call_range_loop(L,"string_byte_range_trace","string_byte_range_value",
                           recorded_end,numeric_end,&actual)==LUA_OK);
    assert(trace_runnable_acq(owner,no) && trace_mcode_acq(owner)==code &&
           trace_szmcode_acq(owner)==codesize && memcmp(saved,code,codesize)==0);
  }
  arm64_fixture_init_trace(&f,L,"string_byte_range_clone",no);
  assert(semantic(&f) && allocated(&f));
  for (IRRef r=REF_FIRST;r<f.semantic_nins;r++) {
    if (f.ir[r].o==IR_FLOAD && f.ir[r].op2==IRFL_STR_LEN) length=r;
    if (length && f.ir[r].o==IR_UGE && f.ir[r].op1==length)
      upper=r, end=f.ir[r].op2;
    if (length && f.ir[r].o==IR_ULE && f.ir[r].op2==length)
      upper=r, end=f.ir[r].op1;
    if (negative_end && f.ir[r].o==IR_LT &&
        f.ir[r].op2<REF_FIRST && f.ir[f.ir[r].op2].o==IR_KINT &&
        f.ir[f.ir[r].op2].i==0)
      upper=r, index=f.ir[r].op1;
    if (negative_end && upper && f.ir[r].o==IR_SUBOV &&
        f.ir[r].op1>=REF_FIRST && f.ir[f.ir[r].op1].o==IR_ADD)
      end=f.ir[r].op1, sum=f.ir[end].op1;
    if (upper && f.ir[r].o==IR_SUBOV && f.ir[r].op1==end)
      difference=r;
    if (difference && f.ir[r].o==IR_EQ && f.ir[r].op1==difference) width=r;
    if (f.ir[r].o==IR_STRREF) pointer=r;
  }
  assert(length && upper && end && difference && width && pointer &&
         length<upper && upper<difference && difference<width && width<pointer);
  if (negative_end) {
    assert(index && sum && upper<sum && sum<end && end<difference &&
           f.ir[sum].o==IR_ADD &&
           ((f.ir[sum].op1==length && f.ir[sum].op2==index) ||
            (f.ir[sum].op2==length && f.ir[sum].op1==index)));
  }
#define REJECT_RANGE(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; \
  assert(!semantic(&f) && !allocated(&f)); \
} while (0)
  REJECT_RANGE(f.ir[upper].t.irt &= (uint8_t)~IRT_GUARD;
               f.postra[upper].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_RANGE(f.ir[difference].t.irt &= (uint8_t)~IRT_GUARD;
               f.postra[difference].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_RANGE(f.ir[width].t.irt &= (uint8_t)~IRT_GUARD;
               f.postra[width].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_RANGE(f.ir[difference].op1=length; f.postra[difference].op1=length);
  REJECT_RANGE(f.ir[pointer].op2=end; f.postra[pointer].op2=end);
  REJECT_RANGE(f.ir[f.ir[width].op2].i=0;
               f.postra[f.postra[width].op2].i=0);
  if (negative_end) {
    REJECT_RANGE(f.ir[sum].op1=length; f.ir[sum].op2=length;
                 f.postra[sum].op1=length; f.postra[sum].op2=length);
    REJECT_RANGE(f.ir[end].op2=f.ir[upper].op2;
                 f.postra[end].op2=f.postra[upper].op2);
    REJECT_RANGE(f.ir[upper].op1=length; f.postra[upper].op1=length);
  }
#undef REJECT_RANGE
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f); lua_pop(L,1); free(saved);
  printf("string.byte(s,2,last=%g): native entry, range/type/empty exits, "
         "GC/code reuse, %u independent range negatives\n",recorded_end,
         negative_end ? 9u : 6u);
}

static void test_dynamic_start_end_byte(lua_State *L, int independent,
                                        int numeric)
{
  Arm64TraceFixture f;
  const char *fn = independent ? "string_byte_pair_trace" :
                                 "string_byte_same_trace";
  const char *clone = independent ? "string_byte_pair_clone" :
                                    "string_byte_same_clone";
  const char *oracle = independent ? "string_byte_pair_oracle" :
                                      "string_byte_same_oracle";
  lua_Number recorded_first = independent ? 2.9 : 2.9;
  lua_Number recorded_last = independent ? 4.9 : 2.9;
  TraceNo no;
  GCtrace *owner;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  IRRef pointer=0, offset=0, first=0, end=0, len=0, upper=0, unrelated=0;
  IRRef fakeconv=0;
  IRRef nonneg=0, difference=0, width=0, r;
  MSize callpos=0, jumppos=0;
  lua_Number actual=0, expected=0;
  static const lua_Number firsts[] = { 2, 1, 2, 4, -2, 2.9 };
  static const lua_Number lasts[] = { 2, 4, 2, 2, -2, 4.9 };
  const char *call = independent ? "byte(s,first,last)" : "byte(s,last,last)";
  char source[2048];

  snprintf(source,sizeof(source),
    "jit.flush(); "
    "%s=function(s,n,%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+%s; i=i+1 end; return sum end; "
    "%s=function(s,n,%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+%s; i=i+1 end; return sum end; "
    "%s=function(s,n,%s) local byte=string.byte; "
    "local i,sum=0,.5; while i<n do sum=sum+%s; i=i+1 end; return sum end; "
    "jit.off(%s,true)",
    fn, independent ? "first,last" : "last", call,
    clone, independent ? "first,last" : "last", call,
    oracle, independent ? "first,last" : "last", call, oracle);
  run_chunk(L,source);
  assert(call_start_end_loop(L,fn,"string_byte_value",recorded_first,
         0,recorded_last,0,&actual)==LUA_OK);
  lua_getglobal(L,fn);
  no=proto_trace_acq(funcproto(funcV(L->top-1))); lua_pop(L,1);
  owner=traceref_safe(L2J(L),no);
  assert(no && owner && trace_runnable_acq(owner,no) &&
         bc_op(trace_startins_acq(owner))==BC_LOOP);
  code=trace_mcode_acq(owner); codesize=trace_szmcode_acq(owner);
  assert(code && codesize); saved=(unsigned char *)malloc(codesize); assert(saved);
  memcpy(saved,code,codesize);
  for (unsigned phase=0;phase<2;phase++) {
    if (phase) lua_gc(L,LUA_GCCOLLECT,0);
    for (unsigned i=0;i<sizeof(firsts)/sizeof(firsts[0]);i++) {
      lua_Number a=independent ? firsts[i] : lasts[i];
      lua_Number b=lasts[i];
      int expected_status=call_start_end_loop(L,oracle,"string_byte_value",
          a, numeric ? 0 : 1, b, numeric ? 0 : 1, &expected);
      lj_trace_test_root_entry_reset(); lj_trace_test_reset_exit_stats();
      int actual_status=call_start_end_loop(L,fn,"string_byte_value",
          a, numeric ? 0 : 1, b, numeric ? 0 : 1, &actual);
      assert(actual_status==expected_status &&
             (actual_status!=LUA_OK || actual==expected));
      assert(lj_trace_test_root_entry_successes()>0 &&
             lj_trace_test_exit_calls()>0 &&
             lj_trace_test_first_exit_parent()==no);
    }
    assert(call_start_end_loop(L,fn,"string_byte_value",recorded_first,
        numeric ? 0 : 1,recorded_last,numeric ? 0 : 1,&actual)==LUA_OK);
    assert(trace_runnable_acq(owner,no) && trace_mcode_acq(owner)==code &&
           trace_szmcode_acq(owner)==codesize && memcmp(saved,code,codesize)==0);
  }
  arm64_fixture_init_trace(&f,L,clone,no);
  f.view.proto=trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  for (r=REF_FIRST;r<f.semantic_nins;r++) {
    IRIns *ir=&f.ir[r];
    if (ir->o==IR_STRREF) pointer=r;
  }
  assert(pointer);
  offset=f.ir[pointer].op2;
  assert(offset>=REF_FIRST && f.ir[offset].o==IR_ADDOV);
  first=f.ir[offset].op1;
  for (r=REF_FIRST;r<pointer;r++) {
    IRIns *ir=&f.ir[r];
    if (ir->o==IR_FLOAD && ir->op2==IRFL_STR_LEN &&
        ir->op1==f.ir[pointer].op1) len=r;
    if (len && ((ir->o==IR_UGE && ir->op1==len) ||
                (ir->o==IR_ULE && ir->op2==len)))
      upper=r, end=ir->o==IR_UGE ? ir->op2 : ir->op1;
    if (ir->o==IR_GE && ir->op1==offset) nonneg=r;
    if (end && ir->o==IR_SUBOV && ir->t.irt==(IRT_INT|IRT_GUARD) &&
        ir->op1==end && ir->op2==offset) difference=r;
    if (difference && ir->o==IR_EQ && ir->t.irt==(IRT_INT|IRT_GUARD) &&
        ir->op1==difference) width=r;
  }
  IRRef first_source=f.ir[first].o==IR_CONV ? f.ir[first].op1 : first;
  IRRef end_source=f.ir[end].o==IR_CONV ? f.ir[end].op1 : end;
  for (r=REF_FIRST;r<pointer;r++) {
    IRIns *ir=&f.ir[r];
    if (r!=first_source && r!=end_source && ir->o==IR_SLOAD &&
        ir->t.irt==(IRT_NUM|IRT_GUARD) &&
        ir->op2==IRSLOAD_TYPECHECK) { unrelated=r; break; }
  }
  for (r=REF_FIRST;r<pointer;r++) {
    IRIns *ir=&f.ir[r];
    if (ir->o==IR_SLOAD && ir->t.irt==(IRT_FUNC|IRT_GUARD) &&
        ir->op2==IRSLOAD_TYPECHECK) { fakeconv=r; break; }
  }
  assert(len && upper && end && nonneg && difference && width &&
         len<upper && upper<difference && nonneg<difference &&
         difference<width && width<pointer &&
         first_source>=REF_FIRST && first_source<offset &&
         end_source>=REF_FIRST && end_source<difference &&
         f.ir[first_source].o==IR_SLOAD && f.ir[end_source].o==IR_SLOAD &&
         f.ir[first_source].op2==IRSLOAD_TYPECHECK &&
         f.ir[end_source].op2==IRSLOAD_TYPECHECK && unrelated && fakeconv);
  assert(lj_trace_test_arm64_string_byte_range_ir(&f.semantic,pointer,
      f.ir[pointer].op1,offset,first,upper));
  assert(lj_trace_test_arm64_string_byte_range_postra(&f.view,pointer,
      f.postra[pointer].op1,offset,first,upper));
  assert(!lj_trace_test_arm64_string_byte_range_ir(&f.semantic,pointer,
      f.ir[pointer].op1,offset,unrelated,upper));
  assert(!lj_trace_test_arm64_string_byte_range_postra(&f.view,pointer,
      f.postra[pointer].op1,offset,unrelated,upper));
  assert(!lj_trace_test_arm64_string_byte_range_ir(&f.semantic,pointer,
      f.ir[pointer].op1,offset,first,nonneg));
  assert(!lj_trace_test_arm64_string_byte_range_postra(&f.view,pointer,
      f.postra[pointer].op1,offset,first,nonneg));
  assert(lj_trace_test_arm64_string_byte_range_index_ir(&f.semantic,first));
  assert(lj_trace_test_arm64_string_byte_range_index_ir(&f.semantic,end));
  assert(lj_trace_test_arm64_string_byte_range_index_postra(&f.view,first));
  assert(lj_trace_test_arm64_string_byte_range_index_postra(&f.view,end));
  assert(lj_trace_test_arm64_string_byte_range_guard_ir(&f.semantic,upper));
  assert(lj_trace_test_arm64_string_byte_range_guard_postra(&f.view,upper));
  if (numeric) {
    arm64_fixture_reset(&f);
    f.ir[fakeconv].o=IR_CONV;
    f.ir[fakeconv].t.irt=IRT_INT;
    f.ir[fakeconv].op1=unrelated;
    f.ir[fakeconv].op2=IRCONV_INT_NUM|IRCONV_ANY;
    f.postra[fakeconv].o=IR_CONV;
    f.postra[fakeconv].t.irt=IRT_INT;
    f.postra[fakeconv].op1=unrelated;
    f.postra[fakeconv].op2=IRCONV_INT_NUM|IRCONV_ANY;
    assert(!lj_trace_test_arm64_string_byte_range_index_ir(&f.semantic,
                                                           fakeconv));
    assert(!lj_trace_test_arm64_string_byte_range_index_postra(&f.view,
                                                               fakeconv));
  }
  arm64_fixture_reset(&f);
  f.ir[fakeconv].o=IR_UGE;
  f.ir[fakeconv].t.irt=IRT_INT|IRT_GUARD;
  f.postra[fakeconv].o=IR_UGE;
  f.postra[fakeconv].t.irt=IRT_INT|IRT_GUARD;
  assert(!lj_trace_test_arm64_string_byte_range_guard_ir(&f.semantic,fakeconv));
  assert(!lj_trace_test_arm64_string_byte_range_guard_postra(&f.view,fakeconv));
  arm64_fixture_reset(&f);
  {
    GCproto *pt=(GCproto *)f.J->pt;
    BCIns *bc=proto_bc(pt), saved_jump=0;
    MSize pc;
    for (pc=0;pc<pt->sizebc;pc++) {
      BCIns ins=bc[pc];
      if (bc_op(ins)==BC_CALL && bc_b(ins)==2u && bc_c(ins)==4u)
        callpos=pc;
    }
    assert(callpos>=3u);
    for (pc=callpos+1u;pc<pt->sizebc;pc++) {
      if (bc_op(bc[pc])==BC_JMP) {
        jumppos=pc; break;
      }
    }
    assert(jumppos);
    saved_jump=bc[jumppos];
    assert(lj_trace_test_arm64_string_byte_range_call_slots(pt,
        f.ir[f.ir[pointer].op1].op1,
        f.ir[first_source].op1,f.ir[end_source].op1));
    setbc_j(&bc[jumppos],(BCPos)((int32_t)callpos-3-(int32_t)jumppos-1));
    assert(lj_trace_test_arm64_string_byte_range_call_slots(pt,
        f.ir[f.ir[pointer].op1].op1,
        f.ir[first_source].op1,f.ir[end_source].op1));
    setbc_j(&bc[jumppos],(BCPos)((int32_t)callpos-2-(int32_t)jumppos-1));
    assert(!lj_trace_test_arm64_string_byte_range_call_slots(pt,
        f.ir[f.ir[pointer].op1].op1,
        f.ir[first_source].op1,f.ir[end_source].op1));
    bc[jumppos]=saved_jump;
  }
  arm64_fixture_reset(&f);
  f.ir[nonneg].o=IR_UGE;
  f.postra[nonneg].o=IR_UGE;
  assert(!lj_trace_test_arm64_string_byte_range_guard_ir(&f.semantic,nonneg));
  assert(!lj_trace_test_arm64_string_byte_range_guard_postra(&f.view,nonneg));
#define REJECT_START_END(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; assert(!semantic(&f) && !allocated(&f)); \
} while (0)
  REJECT_START_END(f.ir[upper].t.irt &= (uint8_t)~IRT_GUARD;
                   f.postra[upper].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_START_END(f.ir[nonneg].t.irt &= (uint8_t)~IRT_GUARD;
                   f.postra[nonneg].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_START_END(f.ir[difference].o=IR_SUB;
                   f.postra[difference].o=IR_SUB);
  REJECT_START_END(f.ir[width].t.irt &= (uint8_t)~IRT_GUARD;
                   f.postra[width].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_START_END(f.ir[len].op1 = f.ir[first].op1;
                   f.postra[len].op1 = f.postra[first].op1);
  if (independent) {
    assert(first_source!=end_source);
    REJECT_START_END(f.ir[first_source].op1 = f.ir[end_source].op1;
                     f.postra[first_source].op1 = f.postra[end_source].op1);
    REJECT_START_END(f.ir[end_source].op1 = f.ir[first_source].op1;
                     f.postra[end_source].op1 = f.postra[first_source].op1);
  } else {
    IRRef source_ref=f.ir[first].o==IR_CONV ? f.ir[first].op1 : first;
    REJECT_START_END(f.ir[source_ref].op1++;
                     f.postra[source_ref].op1++);
  }
#undef REJECT_START_END
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f); free(saved);
  printf("string.byte(s,%s): positive dynamic range, integer/NUMCONV exits, GC/code reuse, source/guard mutations\n",
         independent ? "first,last" : "last,last");
}

static void test_integer_dynamic_index(lua_State *L, int negative)
{
  Arm64TraceFixture f;
  const char *fn = negative ? "string_byte_integer_negative_trace" :
                              "string_byte_integer_trace";
  const char *clone = negative ? "string_byte_integer_negative_clone" :
                                 "string_byte_integer_clone";
  const char *oracle = negative ? "string_byte_integer_negative_oracle" :
                                 "string_byte_integer_oracle";
  lua_Integer recorded = negative ? -2 : 2;
  lua_Number actual = 0.0, expected = 0.0;
  TraceNo no;
  GCtrace *owner;
  MCode *code;
  MSize codesize;
  unsigned char *saved;
  IRRef source = 0, offset = 0, len = 0, upper = 0, nonneg = 0;
  IRRef subov = 0, width = 0, strref = 0, r;
  static const lua_Integer positive_cases[] = { 1, 2, 11, 0, -1 };
  static const lua_Integer negative_cases[] = { -1, -2, -11, 0,
                                                 (lua_Integer)INT32_MIN };
  const lua_Integer *cases = negative ? negative_cases : positive_cases;
  size_t ncases = negative ? sizeof(negative_cases)/sizeof(negative_cases[0]) :
                             sizeof(positive_cases)/sizeof(positive_cases[0]);

  assert(call_integer_index_loop(L, fn, "string_byte_value",
                                 (lua_Number)recorded, 1, &actual) == LUA_OK);
  lua_getglobal(L, fn);
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  owner = traceref_safe(L2J(L), no);
  assert(no && owner && trace_runnable_acq(owner, no) &&
         bc_op(trace_startins_acq(owner)) == BC_LOOP);
  code = trace_mcode_acq(owner); codesize = trace_szmcode_acq(owner);
  assert(code && codesize);
  saved = (unsigned char *)malloc(codesize); assert(saved);
  memcpy(saved, code, codesize);

  for (unsigned phase=0; phase<2; phase++) {
    if (phase) lua_gc(L, LUA_GCCOLLECT, 0);
    for (size_t i=0; i<ncases; i++) {
      int expected_status = call_integer_index_loop(L, oracle,
          "string_byte_value", (lua_Number)cases[i], 1, &expected);
      uint32_t entries = lj_trace_test_root_entry_successes();
      uint32_t exits = lj_trace_test_exit_calls();
      int actual_status = call_integer_index_loop(L, fn,
          "string_byte_value", (lua_Number)cases[i], 1, &actual);
      assert(actual_status == expected_status &&
             (actual_status != LUA_OK || actual == expected));
      assert(lj_trace_test_root_entry_successes() > entries &&
             lj_trace_test_exit_calls() > exits &&
             lj_trace_test_last_exit_parent() == no);
    }
    assert(call_integer_index_loop(L, fn, "string_byte_value",
                                   (lua_Number)recorded, 1,
                                   &actual) == LUA_OK);
    assert(trace_runnable_acq(owner, no) && trace_mcode_acq(owner) == code &&
           trace_szmcode_acq(owner) == codesize &&
           memcmp(saved, code, codesize) == 0);
  }
  {
    lua_Number fractional = negative ? -2.9 : 2.9;
    int expected_status = call_integer_index_loop(L, oracle,
        "string_byte_value", fractional, 0, &expected);
    uint32_t entries = lj_trace_test_root_entry_successes();
    uint32_t exits = lj_trace_test_exit_calls();
    int actual_status = call_integer_index_loop(L, fn,
        "string_byte_value", fractional, 0, &actual);
    assert(actual_status == expected_status &&
           (actual_status != LUA_OK || actual == expected));
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > exits &&
           lj_trace_test_last_exit_parent() == no);
  }

  arm64_fixture_init_trace(&f, L, clone, no);
  f.view.proto = trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  for (r=REF_FIRST; r<f.semantic_nins; r++) {
    if (f.ir[r].o == IR_STRREF) strref = r;
  }
  assert(strref);
  offset = f.ir[strref].op2;
  if (negative) {
    IRRef left = f.ir[offset].op1, right = f.ir[offset].op2;
    if (left >= REF_FIRST && left < offset && f.ir[left].o == IR_FLOAD)
      len = left, source = right;
    else if (right >= REF_FIRST && right < offset && f.ir[right].o == IR_FLOAD)
      len = right, source = left;
    assert(len && source >= REF_FIRST && source < offset);
  } else {
    assert(f.ir[offset].o == IR_ADDOV);
    source = f.ir[offset].op1;
  }
  assert(source >= REF_FIRST && source < offset &&
         f.ir[source].o == IR_SLOAD &&
         f.ir[source].t.irt == (IRT_INT|IRT_GUARD) &&
         f.ir[source].op2 == IRSLOAD_TYPECHECK);
  for (r=REF_FIRST; r<offset; r++) {
    IRIns *ir=&f.ir[r];
    if (negative && ir->o == IR_LT && ir->op1 == source)
      upper = r;
    if (!negative && ((ir->o == IR_ULE && ir->op1 == source) ||
                      (ir->o == IR_UGE && ir->op2 == source)))
      upper = r;
  }
  for (r=offset+1u; r<strref; r++) {
    IRIns *ir=&f.ir[r];
    if (ir->o == IR_GE && ir->op1 == offset) nonneg = r;
    if (ir->o == IR_SUBOV && ir->t.irt == (IRT_INT|IRT_GUARD)) subov = r;
    if (ir->o == IR_EQ && ir->t.irt == (IRT_INT|IRT_GUARD) &&
        subov && ir->op1 == subov) width = r;
  }
  assert(upper && nonneg && subov && width &&
         lj_asm_arm64_test_string_byte_dynamic_index_ir(&f.semantic, source) &&
         lj_asm_arm64_test_string_byte_dynamic_index_postra(&f.view, source));
#define REJECT_INT_INDEX(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; assert(!semantic(&f) && !allocated(&f)); \
} while (0)
  REJECT_INT_INDEX(f.ir[source].op2 = IRSLOAD_READONLY;
                   f.postra[source].op2 = IRSLOAD_READONLY);
  REJECT_INT_INDEX(f.ir[source].op1 = f.ir[strref].op1;
                   f.postra[source].op1 = f.postra[strref].op1);
  REJECT_INT_INDEX(f.ir[source].t.irt = IRT_NUM|IRT_GUARD;
                   f.postra[source].t.irt = IRT_NUM|IRT_GUARD);
  REJECT_INT_INDEX(f.ir[upper].t.irt &= (uint8_t)~IRT_GUARD;
                   f.postra[upper].t.irt &= (uint8_t)~IRT_GUARD);
  REJECT_INT_INDEX(f.ir[nonneg].op1 = source;
                   f.postra[nonneg].op1 = source);
  REJECT_INT_INDEX(f.ir[subov].o = IR_SUB;
                   f.postra[subov].o = IR_SUB);
  REJECT_INT_INDEX(f.ir[width].t.irt &= (uint8_t)~IRT_GUARD;
                   f.postra[width].t.irt &= (uint8_t)~IRT_GUARD);
#undef REJECT_INT_INDEX
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  free(saved);
  printf("string.byte(s,%sint): native root %u, boundary/type exits, GC/reuse, seven semantic/postRA source/bounds negatives\n",
         negative ? "negative-" : "", (unsigned)no);
}

/* Exercise a real raw-U8 snapshot at the comparison exit after string.byte.
** This is deliberately separate from the arithmetic-only dynamic index case:
** the comparison keeps the byte result live in the cold snapshot. */
static void test_u8_snapshot(lua_State *L)
{
  Arm64TraceFixture f;
  GCproto *clonept, *viewpt;
  TraceNo no;
  GCtrace *owner;
  BCIns *clonebc;
  MCode *code;
  MSize codesize, snapno = 0, mapidx = 0, callpc = 0;
  BCReg calla = 0;
  unsigned char *saved;
  unsigned phase;
  IRRef xload = 0, r;
  int found = 0;
  lua_Number actual = 0.0, expected = 0.0;
  static const struct { const char *value; lua_Number index; lua_Integer key; }
    cases[] = {
      { "string_byte_value", 2, 99 },
      { "string_byte_value", 2, 0 },
      { "string_byte_nul", 2, 0 },
      { "string_byte_value", 2.9, 99 },
      { "string_byte_short", 2, 99 },
      { "string_byte_empty", 2, 99 }
    };

  run_chunk(L, "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8')");
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_u8_snapshot_loop(L, "string_byte_u8_snapshot_trace",
                               "string_byte_value", 2, 99, &actual) == LUA_OK);
  lua_getglobal(L, "string_byte_u8_snapshot_trace");
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  owner = traceref_safe(L2J(L), no);
  if (!no || !owner || !trace_runnable_acq(owner, no) ||
      bc_op(trace_startins_acq(owner)) != BC_LOOP)
    fprintf(stderr, "U8 snapshot candidate did not publish: trace=%u owner=%p "
            "starts=%u publishes=%u cleanup=%u aborts=%u err=%u entries=%u\n",
            (unsigned)no, (void *)owner,
            lj_trace_test_root_entry_startins_calls(),
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(), lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            lj_trace_test_root_entry_successes());
  assert(no && owner && trace_runnable_acq(owner, no) &&
         bc_op(trace_startins_acq(owner)) == BC_LOOP);
  code = trace_mcode_acq(owner); codesize = trace_szmcode_acq(owner);
  assert(code && codesize);
  saved = (unsigned char *)malloc(codesize); assert(saved);
  memcpy(saved, code, codesize);

  for (phase = 0; phase < 2; phase++) {
    if (phase) lua_gc(L, LUA_GCCOLLECT, 0);
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
      int expected_status = call_u8_snapshot_loop(L,
          "string_byte_u8_snapshot_oracle", cases[i].value,
          cases[i].index, cases[i].key, &expected);
      uint32_t entries = lj_trace_test_root_entry_successes();
      uint32_t exits = lj_trace_test_exit_calls();
      int actual_status = call_u8_snapshot_loop(L,
          "string_byte_u8_snapshot_trace", cases[i].value,
          cases[i].index, cases[i].key, &actual);
      assert(actual_status == expected_status &&
             (actual_status != LUA_OK || actual == expected));
      assert(lj_trace_test_root_entry_successes() > entries &&
             lj_trace_test_exit_calls() > exits &&
             lj_trace_test_last_exit_parent() == no);
    }
    assert(call_u8_snapshot_loop(L, "string_byte_u8_snapshot_trace",
                                 "string_byte_value", 2, 99,
                                 &actual) == LUA_OK);
    assert(trace_runnable_acq(owner, no) && trace_mcode_acq(owner) == code &&
           trace_szmcode_acq(owner) == codesize &&
           memcmp(saved, code, codesize) == 0);
  }

  lua_getglobal(L, "string_byte_u8_snapshot_clone");
  arm64_fixture_init_trace(&f, L, "string_byte_u8_snapshot_clone", no);
  f.view.proto = trace_startpt_acq(f.owner);
  viewpt = (GCproto *)f.view.proto;
  assert(semantic(&f) && allocated(&f));
  clonept = trace_startpt_acq(&f.semantic);
  clonebc = (BCIns *)(uintptr_t)proto_bc(clonept);
  {
    unsigned calls = 0;
    for (MSize pc = 0; pc < clonept->sizebc; pc++) {
      BCIns ins = clonebc[pc];
      if (bc_op(ins) == BC_CALL && bc_b(ins) == 2 && bc_c(ins) == 3) {
        calls++;
        callpc = pc;
        calla = bc_a(ins);
      }
    }
    assert(calls == 1u);
  }
  for (r = REF_FIRST; r < f.semantic_nins; r++)
    if (f.ir[r].o == IR_XLOAD && f.ir[r].t.irt == IRT_U8) xload = r;
  assert(xload);
  for (SnapNo sn = 0; sn < f.view.nsnap && !found; sn++) {
    const SnapShot *snap = &f.snaps[sn];
    for (MSize i = 0; i < snap_nent_acq(snap); i++) {
      MSize candidate = snap_mapofs_acq(snap) + i;
      SnapEntry entry = f.map[candidate];
      if (snap_ref(entry) == xload) {
        assert((entry & (SNAP_FRAME|SNAP_NORESTORE)) == 0);
        snapno = sn;
        mapidx = candidate;
        found = 1;
        break;
      }
    }
  }
  assert(found);

  /* The real exit maps the direct CALL output slot after its immediate copy. */
  {
    int direct = 0;
    BCIns copy = clonebc[callpc+1u];
    assert((bc_op(copy) == BC_CGET || bc_op(copy) == BC_MOV) &&
           bc_d(copy) == calla && bc_a(copy) != calla);
    for (SnapNo sn = 0; sn < f.view.nsnap; sn++) {
      const SnapShot *snap = &f.snaps[sn];
      uint64_t footer;
      MSize snappc;
      memcpy(&footer, &f.map[snap_mapofs_acq(snap)+snap_nent_acq(snap)],
             sizeof(footer));
      {
        const BCIns *pc = (const BCIns *)(uintptr_t)(footer >> 8);
        uintptr_t lo = (uintptr_t)(const void *)proto_bc(clonept);
        uintptr_t addr = (uintptr_t)(const void *)pc;
        assert(addr >= lo && addr < lo+clonept->sizebc*sizeof(BCIns));
        snappc = (MSize)((addr-lo)/sizeof(BCIns));
      }
      for (MSize i = 0; i < snap_nent_acq(snap); i++) {
        SnapEntry entry = f.map[snap_mapofs_acq(snap)+i];
        MSize reg;
        if (snap_ref(entry) != xload ||
            (entry & (SNAP_FRAME|SNAP_NORESTORE))) continue;
        reg = snap_slot(entry)-1u-LJ_FR2;
        if (reg == calla && snappc > callpc+1u) direct = 1;
      }
    }
    assert(direct);

    /* KNIL range writes before each saved result must invalidate the direct
    ** CALL output provenance, in both independent readers. */
    {
      BCIns old = clonebc[callpc+1u];
      f.view.proto = clonept;
      clonebc[callpc+1u] = BCINS_AD(BC_KNIL, calla, 0);
      arm64_fixture_reset(&f);
      assert(!semantic(&f));
      clonebc[callpc+1u] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
      f.view.proto = clonept;
      clonebc[callpc+1u] = BCINS_AD(BC_KNIL, calla, 0);
      arm64_fixture_reset(&f);
      assert(!allocated(&f));
      clonebc[callpc+1u] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
    }
    {
      BCIns old = clonebc[callpc+2u];
      f.view.proto = clonept;
      clonebc[callpc+2u] = BCINS_AD(BC_KNIL, calla, 0);
      arm64_fixture_reset(&f);
      assert(!semantic(&f));
      clonebc[callpc+2u] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
      f.view.proto = clonept;
      clonebc[callpc+2u] = BCINS_AD(BC_KNIL, calla, 0);
      arm64_fixture_reset(&f);
      assert(!allocated(&f));
      clonebc[callpc+2u] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
    }
    /* Neither a pre-CALL forward edge nor a post-CALL backedge may enter the
    ** saved interval after skipping the value definition. */
    for (phase = 0; phase < 2; phase++) {
      MSize q = phase ? clonept->sizebc : 0;
      BCIns old;
      if (!phase) {
        for (; q < clonept->sizebc; q++)
          if (q < callpc && bc_op(clonebc[q]) == BC_JMP) break;
      } else {
        for (q = clonept->sizebc; q > callpc+1u; q--)
          if (bc_op(clonebc[q-1u]) == BC_JMP) { q--; break; }
      }
      assert(q < clonept->sizebc &&
             (phase ? q > callpc : q < callpc) &&
             bc_op(clonebc[q]) == BC_JMP);
      old = clonebc[q];
      f.view.proto = clonept;
      clonebc[q] = BCINS_AJ(BC_JMP, bc_a(old),
          (BCPos)((int32_t)(callpc+(phase ? 2u : 1u))-(int32_t)(q+1u)));
      arm64_fixture_reset(&f);
      assert(!semantic(&f));
      clonebc[q] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
      f.view.proto = clonept;
      clonebc[q] = BCINS_AJ(BC_JMP, bc_a(old),
          (BCPos)((int32_t)(callpc+(phase ? 2u : 1u))-(int32_t)(q+1u)));
      arm64_fixture_reset(&f);
      assert(!allocated(&f));
      clonebc[q] = old;
      f.view.proto = viewpt;
      arm64_fixture_reset(&f);
      assert(semantic(&f) && allocated(&f));
    }
    f.view.proto = viewpt;
  }

  /* Each view accepts the captured exact map first. Then mutate one side at a
  ** time, so semantic and allocated proofs cannot borrow each other's state. */
  {
    SnapEntry old = f.map[mapidx], postold = f.postmap[mapidx];
    MSize prev = 0, next = snap_nslots_acq(&f.snaps[snapno]);
    MSize i = mapidx - snap_mapofs_acq(&f.snaps[snapno]);
    MSize other_slot = snap_slot(old);
    if (i) prev = snap_slot(f.map[mapidx-1u]);
    if (i+1u < snap_nent_acq(&f.snaps[snapno]))
      next = snap_slot(f.map[mapidx+1u]);
    if (snap_slot(old)+1u < next) other_slot++;
    else if (snap_slot(old) > prev+1u) other_slot--;
    else assert(!"U8 snapshot has no adjacent unmapped slot");
    f.map[mapidx] = snap_with_slot(old, other_slot);
    assert(!semantic(&f) && allocated(&f));
    f.map[mapidx] = old;
    f.postmap[mapidx] = snap_with_slot(postold, other_slot);
    assert(semantic(&f) && !allocated(&f));
    f.postmap[mapidx] = postold;
  }
  {
    IRIns old = f.postra[xload];
    f.postra[xload].r = RID_D0;
    f.postra[xload].s = SPS_NONE;
    assert(semantic(&f) && !allocated(&f));
    f.postra[xload] = old;
  }
  {
    IRIns old = f.postra[xload];
    MSize oldadjust = f.view.spadjust, slot = 0, highest = 0;
    /* Build a private, internally consistent one-slot spill assignment at
    ** the end of an aligned spill block. This exercises U8's exact spill
    ** width and lack of 64-bit alignment independently of allocator luck. */
    for (MSize candidate = SPS_FIXED + 1u; candidate + 1u < SPS_LIMIT;
         candidate += 2u) {
      MSize high = candidate + 1u;
      int free_slot = 1;
      for (IRRef q = REF_FIRST; q < f.view.nins; q++) {
        IRIns ins = f.postra[q];
        MSize width, end;
        if (!ra_hasspill(ins.s)) continue;
        width = irt_type(ins.t) == IRT_INT || irt_type(ins.t) == IRT_U8 ?
                1u : 2u;
        end = (MSize)ins.s + width;
        if (ins.s <= candidate && candidate < end) free_slot = 0;
        if (candidate <= ins.s && ins.s < candidate + 1u) free_slot = 0;
        if (end > high) high = end;
      }
      if (free_slot && high == candidate + 1u &&
          SPS_FIXED + sps_align(high) == candidate + 1u) {
        slot = candidate;
        highest = high;
        break;
      }
    }
    assert(slot && (slot & 1u) && highest == slot + 1u);
    f.postra[xload].r = RID_INIT;
    f.postra[xload].s = (uint8_t)slot;
    f.postra[xload].prev = REGSP(RID_INIT, slot);
    f.view.spadjust = sps_scale(sps_align(highest));
    assert(semantic(&f) && allocated(&f));
    f.postra[xload].s = UINT8_MAX;
    f.postra[xload].prev = REGSP(RID_INIT, UINT8_MAX);
    assert(semantic(&f) && !allocated(&f));
    f.postra[xload] = old;
    f.view.spadjust = oldadjust;
  }
  {
    IRRef rename = 0;
    for (r = f.semantic_nins; r < f.nins; r++)
      if (f.postra[r].o == IR_RENAME) { rename = r; break; }
    assert(rename);
    {
      IRIns old = f.postra[rename];
      f.postra[rename].op1 = xload;
      f.postra[rename].r = RID_D0;
      assert(semantic(&f) && !allocated(&f));
      f.postra[rename] = old;
    }
  }
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  lua_pop(L, 1);
  free(saved);
  printf("string.byte raw U8 snapshot: native root %u, cold/type/bounds exits, GC/code reuse, independent source/postRA mutations\n",
         (unsigned)no);
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
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=8')\n"
    "string_byte_trace=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte(s,2) end; return sum\n"
    "end\n"
    "string_byte_trace_clone=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte(s,2) end; return sum\n"
    "end\n"
    "string_byte_literal_trace=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte('abc',2) end; return sum\n"
    "end\n"
    "string_byte_literal_trace_clone=function(s,n)\n"
    " local byte=string.byte; local sum=0; "
    "for i=1,n do sum=sum+byte('abc',2) end; return sum\n"
    "end\n"
    "string_byte_dynamic_trace=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_dynamic_trace_clone=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_u8_snapshot_trace=function(s,n,index,k)\n"
    " local byte=string.byte; local i,sum=1,.5; "
    "while i<=n do local a=byte(s,index); "
    "if a==k then sum=sum+a*1.25 else sum=sum+a*2.25 end; "
    "i=i+1 end; return sum\n"
    "end\n"
    "string_byte_u8_snapshot_clone=function(s,n,index,k)\n"
    " local byte=string.byte; local i,sum=1,.5; "
    "while i<=n do local a=byte(s,index); "
    "if a==k then sum=sum+a*1.25 else sum=sum+a*2.25 end; "
    "i=i+1 end; return sum\n"
    "end\n"
    "string_byte_u8_snapshot_oracle=function(s,n,index,k)\n"
    " local byte=string.byte; local i,sum=1,.5; "
    "while i<=n do local a=byte(s,index); "
    "if a==k then sum=sum+a*1.25 else sum=sum+a*2.25 end; "
    "i=i+1 end; return sum\n"
    "end\n"
    "string_byte_negative_dynamic_trace=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_negative_dynamic_trace_clone=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_negative_constant_trace=function(byte,s,n)\n"
    " local i,sum=0,.5; while i<n do sum=sum+byte(s,-2); i=i+1 end; "
    "return sum end\n"
    "string_byte_negative_constant_clone=function(byte,s,n)\n"
    " local i,sum=0,.5; while i<n do sum=sum+byte(s,-2); i=i+1 end; "
    "return sum end\n"
    "string_byte_negative_constant_oracle=function(byte,s,n)\n"
    " local i,sum=0,.5; while i<n do sum=sum+byte(s,-2); i=i+1 end; "
    "return sum end\n"
    "jit.off(string_byte_negative_constant_oracle,true)\n"
    "string_byte_builtin=string.byte\n"
    "string_byte_negative_constant_value=string.rep('const-negative-',24)\n"
    "string_byte_negative_constant_nul='\\255\\0x'\n"
    "string_byte_negative_constant_high='\\0\\255x'\n"
    "string_byte_negative_constant_pair='\\0\\255'\n"
    "string_byte_negative_constant_short='x'\n"
    "string_byte_negative_constant_empty=''\n"
    "string_byte_integer_trace=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_integer_clone=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_integer_oracle=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_integer_negative_trace=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_integer_negative_clone=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_integer_negative_oracle=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0.5; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "string_byte_dynamic_oracle=function(s,n,index)\n"
    " local byte=string.byte; local i,sum=1,0; "
    "while i<=n do sum=sum+byte(s,index); i=i+1 end; return sum\n"
    "end\n"
    "jit.off(string_byte_dynamic_oracle,true)\n"
    "jit.off(string_byte_u8_snapshot_oracle,true)\n"
    "jit.off(string_byte_integer_oracle,true); "
    "jit.off(string_byte_integer_negative_oracle,true)\n"
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
    "assert(string_byte_dynamic_trace(string_byte_value,80,2.9)=="
    "string.byte('gcstr-byte-',2)*80)\n"
    "assert(string_byte_negative_dynamic_trace(string_byte_value,80,-2.9)=="
    "string.byte('gcstr-byte-',-2)*80)\n"
    "assert(string_byte_negative_constant_trace(string.byte,"
    "string_byte_negative_constant_value,80)=="
    "string_byte_negative_constant_oracle(string.byte,"
    "string_byte_negative_constant_value,80))\n"
    "assert(string_byte_integer_trace(string_byte_value,80,2)=="
    "string.byte('gcstr-byte-',2)*80+0.5)\n"
    "assert(string_byte_integer_negative_trace(string_byte_value,80,-2)=="
    "string.byte('gcstr-byte-',-2)*80+0.5)\n"
    "assert(string_byte_bounds_trace(string_byte_value,80)=="
    "string.byte('gcstr-byte-',2)*80+0.5)\n";
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  GCproto *wrong_proto;
  GCtrace *owner, *bounds_owner, *dynamic_owner, *negative_owner;
  GCtrace *negative_constant_owner;
  const IRIns *ir;
  IRRef r, source_ref = 0, len_ref = 0, guard_ref = 0;
  IRRef strref = 0, xload = 0;
  MSize s;
  unsigned mapped_source = 0;
  uint32_t entries, exits;
  TraceNo bounds_no, dynamic_no, negative_no, negative_constant_no;
  TraceNo literal_no = 0;
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

  lua_getglobal(L, "string_byte_dynamic_trace");
  dynamic_no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  dynamic_owner = traceref_safe(L2J(L), dynamic_no);
  if (dynamic_no == 0 || !dynamic_owner ||
      !trace_runnable_acq(dynamic_owner, dynamic_no))
    fprintf(stderr, "dynamic string-byte no native root: trace=%u owner=%p "
            "starts=%u publishes=%u cleanups=%u aborts=%u error=%u "
            "entries=%u exits=%u errinfo=%u (ULE=%u ULT=%u GE=%u "
            "CONV=%u ADDOV=%u)\n", (unsigned)dynamic_no,
            (void *)dynamic_owner, lj_trace_test_root_entry_startins_calls(),
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(), lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            lj_trace_test_root_entry_successes(), lj_trace_test_exit_calls(),
            tvisint(&L2J(L)->errinfo) ? (unsigned)intV(&L2J(L)->errinfo) : 0u,
            (unsigned)IR_ULE, (unsigned)IR_ULT, (unsigned)IR_GE,
            (unsigned)IR_CONV, (unsigned)IR_ADDOV);
  assert(dynamic_no != 0 && dynamic_owner &&
         trace_runnable_acq(dynamic_owner, dynamic_no) &&
         bc_op(trace_startins_acq(dynamic_owner)) == BC_LOOP);
  lua_getglobal(L, "string_byte_negative_dynamic_trace");
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  negative_no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  negative_owner = traceref_safe(L2J(L), negative_no);
  if (negative_no == 0 || !negative_owner ||
      !trace_runnable_acq(negative_owner, negative_no))
    fprintf(stderr, "negative string-byte no native root: trace=%u "
            "owner=%p starts=%u publishes=%u cleanups=%u aborts=%u "
            "error=%u errinfo=%u entries=%u exits=%u\n",
            (unsigned)negative_no,
            (void *)negative_owner,
            lj_trace_test_root_entry_startins_calls(),
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(), lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            tvisint(&L2J(L)->errinfo) ? (unsigned)intV(&L2J(L)->errinfo) : 0u,
            lj_trace_test_root_entry_successes(),
            lj_trace_test_exit_calls());
  assert(negative_no != 0 && negative_owner &&
         trace_runnable_acq(negative_owner, negative_no) &&
         bc_op(trace_startins_acq(negative_owner)) == BC_LOOP);
  lua_getglobal(L, "string_byte_negative_constant_trace");
  negative_constant_no = proto_trace_acq(funcproto(funcV(L->top-1)));
  lua_pop(L, 1);
  negative_constant_owner = traceref_safe(L2J(L), negative_constant_no);
  if (negative_constant_no == 0 || !negative_constant_owner ||
      !trace_runnable_acq(negative_constant_owner, negative_constant_no))
    fprintf(stderr, "constant negative string-byte no native root: trace=%u "
            "owner=%p starts=%u publishes=%u cleanups=%u aborts=%u "
            "error=%u entries=%u exits=%u\n",
            (unsigned)negative_constant_no, (void *)negative_constant_owner,
            lj_trace_test_root_entry_startins_calls(),
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(), lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            lj_trace_test_root_entry_successes(), lj_trace_test_exit_calls());
  assert(negative_constant_no != 0 && negative_constant_owner &&
         trace_runnable_acq(negative_constant_owner, negative_constant_no) &&
         bc_op(trace_startins_acq(negative_constant_owner)) == BC_LOOP);
  lua_getglobal(L, "string_byte_trace");
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  wrong_proto = funcproto(funcV(L->top-1));
  lua_pop(L, 1);

  /* Hold a second TG live while collection and all successful byte loads run. */
  run_chunk(L, "collectgarbage('collect')");
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_trace", "string_byte_value", &value) == LUA_OK &&
         value == 99.0*80.0);
  assert(lj_trace_test_root_entry_successes() > entries);
  entries = lj_trace_test_root_entry_successes();
  assert(call_dynamic_loop(L, "string_byte_dynamic_trace", 2.9, &value) ==
         LUA_OK && value == 99.0*80.0);
  assert(lj_trace_test_root_entry_successes() > entries);
  entries = lj_trace_test_root_entry_successes();
  {
    int status = call_dynamic_loop(L, "string_byte_negative_dynamic_trace",
                                   -2.9, &value);
    if (status != LUA_OK || value != 101.0*80.0)
      fprintf(stderr, "negative dynamic byte result status=%d value=%.17g "
              "expected=%.17g entries=%u exits=%u parent=%u trace=%u\n",
              status, value, 101.0*80.0,
              lj_trace_test_root_entry_successes(), lj_trace_test_exit_calls(),
              (unsigned)lj_trace_test_last_exit_parent(),
              (unsigned)negative_no);
    assert(status == LUA_OK && value == 101.0*80.0);
  }
  assert(lj_trace_test_root_entry_successes() > entries);
  {
    unsigned old_entries = lj_trace_test_root_entry_successes();
    unsigned old_exits = lj_trace_test_exit_calls();
    int status = call_dynamic_loop_value(
        L, "string_byte_negative_dynamic_trace", "string_byte_short", -2,
        &value);
    if (status == LUA_OK ||
        lj_trace_test_root_entry_successes() <= old_entries ||
        lj_trace_test_exit_calls() <= old_exits ||
        lj_trace_test_last_exit_parent() != negative_no)
      fprintf(stderr, "negative string-byte bound exit status=%d "
              "entries=%u/%u exits=%u/%u last-parent=%u trace=%u\n",
              status, old_entries, lj_trace_test_root_entry_successes(),
              old_exits, lj_trace_test_exit_calls(),
              (unsigned)lj_trace_test_last_exit_parent(),
              (unsigned)negative_no);
    assert(status != LUA_OK &&
           lj_trace_test_root_entry_successes() > old_entries &&
           lj_trace_test_exit_calls() > old_exits &&
           lj_trace_test_last_exit_parent() == negative_no);
  }
  {
    static const char *const valid[] = {
      "string_byte_negative_constant_value",
      "string_byte_negative_constant_nul",
      "string_byte_negative_constant_high",
      "string_byte_negative_constant_pair"
    };
    MCode *code = trace_mcode_acq(negative_constant_owner);
    MSize codesize = trace_szmcode_acq(negative_constant_owner);
    unsigned char *saved = (unsigned char *)malloc(codesize);
    unsigned i;
    assert(code && codesize && saved);
    memcpy(saved, code, codesize);
    for (i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
      lua_Number expected = 0.0, actual = 0.0;
      int expected_status = call_negative_constant_loop(
          L, "string_byte_negative_constant_oracle", valid[i], &expected);
      entries = lj_trace_test_root_entry_successes();
      assert(expected_status == LUA_OK);
      assert(call_negative_constant_loop(
          L, "string_byte_negative_constant_trace", valid[i], &actual) ==
          expected_status && actual == expected);
      assert(lj_trace_test_root_entry_successes() > entries);
    }
    assert(trace_runnable_acq(negative_constant_owner, negative_constant_no) &&
           trace_mcode_acq(negative_constant_owner) == code &&
           trace_szmcode_acq(negative_constant_owner) == codesize &&
           memcmp(saved, code, codesize) == 0);
    run_chunk(L, "collectgarbage('collect')");
    {
      lua_Number expected = 0.0, actual = 0.0;
      unsigned old_entries = lj_trace_test_root_entry_successes();
      assert(call_negative_constant_loop(
          L, "string_byte_negative_constant_oracle",
          "string_byte_negative_constant_high", &expected) == LUA_OK);
      assert(call_negative_constant_loop(
          L, "string_byte_negative_constant_trace",
          "string_byte_negative_constant_high", &actual) == LUA_OK &&
          actual == expected);
      assert(lj_trace_test_root_entry_successes() > old_entries);
    }
    assert(trace_runnable_acq(negative_constant_owner, negative_constant_no) &&
           trace_mcode_acq(negative_constant_owner) == code &&
           trace_szmcode_acq(negative_constant_owner) == codesize &&
           memcmp(saved, code, codesize) == 0);
    entries = lj_trace_test_root_entry_successes();
    assert(call_negative_constant_loop(
        L, "string_byte_negative_constant_oracle",
        "string_byte_negative_constant_short", &value) != LUA_OK);
    lj_trace_test_reset_exit_stats();
    exits = lj_trace_test_exit_calls();
    assert(call_negative_constant_loop(
        L, "string_byte_negative_constant_trace",
        "string_byte_negative_constant_short", &value) != LUA_OK);
    if (!(lj_trace_test_root_entry_successes() > entries &&
          lj_trace_test_exit_calls() > exits &&
          lj_trace_test_first_exit_parent() == negative_constant_no))
      fprintf(stderr, "constant negative short exit entries=%u/%u exits=%u/%u "
              "parent=%u trace=%u\n", entries,
              lj_trace_test_root_entry_successes(), exits,
              lj_trace_test_exit_calls(),
              (unsigned)lj_trace_test_first_exit_parent(),
              (unsigned)negative_constant_no);
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > exits &&
           lj_trace_test_first_exit_parent() == negative_constant_no);
    entries = lj_trace_test_root_entry_successes();
    assert(call_negative_constant_loop(
        L, "string_byte_negative_constant_oracle",
        "string_byte_negative_constant_empty", &value) != LUA_OK);
    lj_trace_test_reset_exit_stats();
    exits = lj_trace_test_exit_calls();
    assert(call_negative_constant_loop(
        L, "string_byte_negative_constant_trace",
        "string_byte_negative_constant_empty", &value) != LUA_OK);
    assert(lj_trace_test_root_entry_successes() > entries &&
           lj_trace_test_exit_calls() > exits &&
           lj_trace_test_first_exit_parent() == negative_constant_no);
    free(saved);
  }
  /* Constant-negative proof binds the KSHORT source argument, length base,
  ** lower-bound guard, checked one-byte range and STRREF independently. */
  arm64_fixture_init_trace(&f, L, "string_byte_negative_constant_clone",
                           negative_constant_no);
  f.view.proto = trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  {
    IRRef string = 0, lenref = 0, strref = 0, offset = 0;
    IRRef indexref = 0, endref = 0, endindexref = 0;
    IRRef nonneg = 0, subref = 0, width = 0;
    int32_t index = 0;
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      if (!string && f.ir[r].o == IR_SLOAD &&
          f.ir[r].t.irt == (IRT_STR|IRT_GUARD) &&
          f.ir[r].op2 == IRSLOAD_TYPECHECK) string = r;
      if (!lenref && string && f.ir[r].o == IR_FLOAD &&
          f.ir[r].t.irt == IRT_INT && f.ir[r].op1 == string &&
          f.ir[r].op2 == IRFL_STR_LEN) lenref = r;
      if (f.ir[r].o == IR_STRREF && f.ir[r].t.irt == IRT_PGC)
        strref = r, offset = f.ir[r].op2;
    }
    assert(string && lenref && strref && offset);
    assert(f.ir[offset].o == IR_ADD && f.ir[offset].t.irt == IRT_INT);
    if (f.ir[offset].op1 == lenref) indexref = f.ir[offset].op2;
    else if (f.ir[offset].op2 == lenref) indexref = f.ir[offset].op1;
    assert(indexref && kint(f.ir, f.semantic.nk, indexref, &index) &&
           index == -2);
    for (r = offset+1u; r < strref; r++) {
      int32_t k;
      if (!endref && f.ir[r].o == IR_ADD && f.ir[r].t.irt == IRT_INT &&
          ((f.ir[r].op1 == lenref &&
            kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == index+1 &&
            (endindexref = f.ir[r].op2, 1)) ||
           (f.ir[r].op2 == lenref &&
            kint(f.ir, f.semantic.nk, f.ir[r].op1, &k) && k == index+1 &&
            (endindexref = f.ir[r].op1, 1))))
        endref = r;
      if (!nonneg && f.ir[r].o == IR_GE &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          f.ir[r].op1 == offset &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 0)
        nonneg = r;
      if (!subref && endref && f.ir[r].o == IR_SUBOV &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          f.ir[r].op1 == endref && f.ir[r].op2 == offset) subref = r;
      if (!width && subref && f.ir[r].o == IR_EQ &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          f.ir[r].op1 == subref &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 1)
        width = r;
    }
    assert(endref && endindexref && nonneg && subref && width &&
           offset < nonneg && nonneg < subref && subref < width &&
           width < strref);
#define REJECT_CONST(change) do { \
  int sem_ok, post_ok; \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; sem_ok = semantic(&f); post_ok = allocated(&f); \
  if (sem_ok || post_ok) { \
    fprintf(stderr, "constant-negative string-byte mutation accepted at " \
                    "line %d (semantic=%d postRA=%d)\n", \
            __LINE__, sem_ok, post_ok); abort(); \
  } \
} while (0)
    REJECT_CONST(f.ir[string].op1++; f.postra[string].op1++);
    REJECT_CONST(f.ir[lenref].op2++; f.postra[lenref].op2++);
    REJECT_CONST(f.ir[indexref].i++; f.postra[indexref].i++);
    REJECT_CONST(f.ir[endindexref].i++; f.postra[endindexref].i++);
    REJECT_CONST(f.ir[offset].op1 = indexref;
                 f.postra[offset].op1 = indexref);
    REJECT_CONST(f.ir[nonneg].o = IR_GT; f.postra[nonneg].o = IR_GT);
    REJECT_CONST(f.ir[subref].op2 = indexref;
                 f.postra[subref].op2 = indexref);
    REJECT_CONST(f.ir[width].op1 = offset; f.postra[width].op1 = offset);
    REJECT_CONST(f.ir[strref].op1 = lenref; f.postra[strref].op1 = lenref);
#undef REJECT_CONST
  }
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  printf("string.byte(s,-2): native NUL/high-byte, short/empty exits, "
         "GC/code reuse and nine semantic/post-RA source/range negatives\n");
  {
    const IRIns *dynamic_ir = trace_ir_acq(dynamic_owner);
    IRRef conv = 0, addov = 0, upper = 0, nonneg = 0, width = 0;
    IRRef subov = 0, dynamic_strref = 0, lenref = 0;
    IRRef nk = trace_nk_acq(dynamic_owner);
    for (r = REF_FIRST; r < trace_nins_acq(dynamic_owner); r++) {
      int32_t k;
      if (!conv && dynamic_ir[r].o == IR_CONV &&
          dynamic_ir[r].t.irt == IRT_INT &&
          dynamic_ir[r].op2 == (IRCONV_INT_NUM|IRCONV_ANY)) conv = r;
      if (dynamic_ir[r].o == IR_FLOAD && dynamic_ir[r].op2 == IRFL_STR_LEN)
        lenref = r;
      if (!addov && dynamic_ir[r].o == IR_ADDOV &&
          dynamic_ir[r].t.irt == (IRT_INT|IRT_GUARD) && conv &&
          dynamic_ir[r].op1 == conv &&
          kint(dynamic_ir, nk, dynamic_ir[r].op2, &k) && k == -1)
        addov = r;
      if (!upper && dynamic_ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          ((dynamic_ir[r].o == IR_ULE && dynamic_ir[r].op1 == conv &&
            dynamic_ir[r].op2 == lenref) ||
           (dynamic_ir[r].o == IR_UGE && dynamic_ir[r].op1 == lenref &&
            dynamic_ir[r].op2 == conv))) upper = r;
      if (!nonneg && dynamic_ir[r].o == IR_GE &&
          dynamic_ir[r].t.irt == (IRT_INT|IRT_GUARD) && addov &&
          dynamic_ir[r].op1 == addov &&
          kint(dynamic_ir, nk, dynamic_ir[r].op2, &k) && k == 0)
        nonneg = r;
      if (!subov && dynamic_ir[r].o == IR_SUBOV &&
          dynamic_ir[r].t.irt == (IRT_INT|IRT_GUARD) && addov &&
          dynamic_ir[r].op1 == conv && dynamic_ir[r].op2 == addov)
        subov = r;
      if (!width && dynamic_ir[r].o == IR_EQ &&
          dynamic_ir[r].t.irt == (IRT_INT|IRT_GUARD) && subov &&
          dynamic_ir[r].op1 == subov &&
          kint(dynamic_ir, nk, dynamic_ir[r].op2, &k) && k == 1)
        width = r;
      if (dynamic_ir[r].o == IR_STRREF && dynamic_ir[r].op2 == addov)
        dynamic_strref = r;
    }
    assert(conv && addov && upper && nonneg && subov && width &&
           dynamic_strref && conv < upper && upper < addov &&
           addov < nonneg && nonneg < subov && subov < width &&
           width < dynamic_strref);
    assert(dynamic_ir[addov].op1 == conv &&
           dynamic_ir[subov].op1 == conv &&
           dynamic_ir[subov].op2 == addov &&
           dynamic_ir[nonneg].op1 == addov &&
           dynamic_ir[dynamic_strref].op2 == addov);
  }
  entries = lj_trace_test_root_entry_successes();
  exits = lj_trace_test_exit_calls();
  assert(call_dynamic_loop(L, "string_byte_dynamic_trace", 0.0, &value) !=
         LUA_OK);
  assert(lj_trace_test_root_entry_successes() > entries &&
         lj_trace_test_exit_calls() > exits);
  entries = lj_trace_test_root_entry_successes();
  exits = lj_trace_test_exit_calls();
  assert(call_dynamic_loop(L, "string_byte_dynamic_trace", -1.0, &value) ==
         LUA_OK && value == 45.0*80.0);
  assert(lj_trace_test_root_entry_successes() > entries &&
         lj_trace_test_exit_calls() > exits);
  {
    const lua_Number invalid[] = { -0.0, NAN, INFINITY, -INFINITY,
                                   2147483648.0, -2147483649.0,
                                   1.0e300, -1.0e300 };
    unsigned n;
    for (n = 0; n < sizeof(invalid)/sizeof(invalid[0]); n++) {
      entries = lj_trace_test_root_entry_successes();
      exits = lj_trace_test_exit_calls();
      assert(call_dynamic_loop(L, "string_byte_dynamic_trace", invalid[n],
                               &value) != LUA_OK);
      assert(lj_trace_test_root_entry_successes() > entries &&
             lj_trace_test_exit_calls() > exits &&
             lj_trace_test_last_exit_parent() == dynamic_no);
    }
  }
  /* Exercise the negative-origin trace against the interpreter, including
  ** truncation at the first byte and saturated conversions. Each call must
  ** enter and exit this selected trace, even when it resumes with an error. */
  {
    static const struct {
      const char *string;
      lua_Number index;
    } cases[] = {
      { "string_byte_value", -1.0 },
      { "string_byte_value", -2.9 },
      { "string_byte_value", -240.0 },
      { "string_byte_value", -240.9 },
      { "string_byte_value", -241.0 },
      { "string_byte_value", -2147483648.0 },
      { "string_byte_value", -2147483649.0 },
      { "string_byte_value", 2147483647.0 },
      { "string_byte_value", 2147483648.0 },
      { "string_byte_value", -1.0e300 },
      { "string_byte_value", 1.0e300 },
      { "string_byte_value", -INFINITY },
      { "string_byte_value", INFINITY },
      { "string_byte_value", NAN },
      { "string_byte_value", -0.0 },
      { "string_byte_value", 0.0 },
      { "string_byte_value", 2.9 },
      { "string_byte_short", -1.0 },
      { "string_byte_short", -1.9 },
      { "string_byte_short", -2.0 },
      { "string_byte_empty", -1.0 },
      { "string_byte_empty", -2147483648.0 },
      { "string_byte_nul", -1.0 },
      { "string_byte_nul", -2.9 },
      { "string_byte_nul", -3.9 },
      { "string_byte_nul", -4.0 },
      { "string_byte_long", -65536.9 },
      { "string_byte_long", -65537.0 }
    };
    unsigned n;
    for (n = 0; n < sizeof(cases)/sizeof(cases[0]); n++) {
      lua_Number expected = 0.0, actual = 0.0;
      int expected_status = call_dynamic_loop_value(L,
          "string_byte_dynamic_oracle", cases[n].string, cases[n].index,
          &expected);
      int actual_status;
      entries = lj_trace_test_root_entry_successes();
      exits = lj_trace_test_exit_calls();
      actual_status = call_dynamic_loop_value(L,
          "string_byte_negative_dynamic_trace", cases[n].string,
          cases[n].index, &actual);
      if (actual_status != expected_status ||
          (actual_status == LUA_OK && actual != expected))
        fprintf(stderr, "negative byte oracle case=%u index=%.17g "
                "status=%d/%d value=%.17g/%.17g\n", n, cases[n].index,
                actual_status, expected_status, actual, expected);
      assert(actual_status == expected_status &&
             (actual_status != LUA_OK || actual == expected));
      assert(lj_trace_test_root_entry_successes() > entries &&
             lj_trace_test_exit_calls() > exits &&
             lj_trace_test_last_exit_parent() == negative_no);
    }
  }
  /* The negative form uses len+index. Bind every guard and the one-byte
  ** width to the same conversion and string in both independent views. */
  arm64_fixture_init_trace(&f, L, "string_byte_negative_dynamic_trace_clone",
                           negative_no);
  f.view.proto = trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  {
    IRRef conv = 0, string = 0, lenref = 0, negative = 0, offset = 0;
    IRRef nonneg = 0, endref = 0, subref = 0, width = 0, strref = 0;
    IRRef one = 0;
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      int32_t k;
      if (!conv && f.ir[r].o == IR_CONV && f.ir[r].t.irt == IRT_INT &&
          f.ir[r].op2 == (IRCONV_INT_NUM|IRCONV_ANY)) conv = r;
      if (!string && f.ir[r].o == IR_SLOAD &&
          f.ir[r].t.irt == (IRT_STR|IRT_GUARD) &&
          f.ir[r].op2 == IRSLOAD_TYPECHECK) string = r;
      if (string && !lenref && f.ir[r].o == IR_FLOAD &&
          f.ir[r].t.irt == IRT_INT && f.ir[r].op1 == string &&
          f.ir[r].op2 == IRFL_STR_LEN) lenref = r;
      if (conv && !negative && f.ir[r].o == IR_LT &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == conv &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 0)
        negative = r;
      if (conv && lenref && !offset && f.ir[r].o == IR_ADD &&
          f.ir[r].t.irt == IRT_INT &&
          ((f.ir[r].op1 == lenref && f.ir[r].op2 == conv) ||
           (f.ir[r].op1 == conv && f.ir[r].op2 == lenref))) offset = r;
      if (offset && !nonneg && f.ir[r].o == IR_GE &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == offset &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 0)
        nonneg = r;
      if (offset && !endref && f.ir[r].o == IR_ADD &&
          f.ir[r].t.irt == IRT_INT && f.ir[r].op1 == offset &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 1)
        endref = r, one = f.ir[r].op2;
      if (endref && !subref && f.ir[r].o == IR_SUBOV &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          f.ir[r].op1 == endref && f.ir[r].op2 == offset)
        subref = r;
      if (subref && !width && f.ir[r].o == IR_EQ &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == subref &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 1)
        width = r;
      if (offset && f.ir[r].o == IR_STRREF &&
          f.ir[r].t.irt == IRT_PGC && f.ir[r].op2 == offset)
        strref = r;
    }
    assert(conv && string && lenref && negative && offset && nonneg &&
           endref && subref && width && strref && one);
    assert(conv < negative && negative < offset && offset < endref &&
           endref < nonneg && nonneg < subref && subref < width &&
           width < strref);
    assert(((f.ir[offset].op1 == lenref && f.ir[offset].op2 == conv) ||
            (f.ir[offset].op1 == conv && f.ir[offset].op2 == lenref)) &&
           f.ir[endref].op1 == offset && f.ir[subref].op1 == endref &&
           f.ir[subref].op2 == offset && f.ir[strref].op2 == offset);
    {
      LJArm64PostRAView wrong_view = f.view;
      wrong_view.proto = wrong_proto;
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_postra(
          &wrong_view, conv));
      assert(lj_asm_arm64_test_string_byte_dynamic_index_postra(
          &f.view, conv));
    }
#define REJECT_NEGATIVE(change) do { \
  int sem_ok, post_ok; \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; sem_ok = semantic(&f); post_ok = allocated(&f); \
  if (sem_ok || post_ok) { \
    fprintf(stderr, "negative string-byte mutation accepted at line %d " \
                    "(semantic=%d postRA=%d)\n", __LINE__, sem_ok, post_ok); \
    abort(); \
  } \
} while (0)
    REJECT_NEGATIVE(f.ir[negative].o = IR_GE;
                    f.postra[negative].o = IR_GE);
    REJECT_NEGATIVE(f.ir[negative].op1 = lenref;
                    f.postra[negative].op1 = lenref);
    REJECT_NEGATIVE(f.ir[offset].op1 = string;
                    f.postra[offset].op1 = string);
    REJECT_NEGATIVE(f.ir[offset].op2 = conv;
                    f.postra[offset].op2 = conv);
    REJECT_NEGATIVE(f.ir[lenref].op1 = conv;
                    f.postra[lenref].op1 = conv);
    REJECT_NEGATIVE(f.ir[nonneg].op1 = conv;
                    f.postra[nonneg].op1 = conv);
    REJECT_NEGATIVE(f.ir[endref].op2 = conv;
                    f.postra[endref].op2 = conv);
    REJECT_NEGATIVE(f.ir[subref].op1 = offset;
                    f.postra[subref].op1 = offset);
    REJECT_NEGATIVE(f.ir[subref].op2 = conv;
                    f.postra[subref].op2 = conv);
    REJECT_NEGATIVE(f.ir[width].op2 = offset;
                    f.postra[width].op2 = offset);
    REJECT_NEGATIVE(f.ir[strref].op2 = conv;
                    f.postra[strref].op2 = conv);
#undef REJECT_NEGATIVE
  }
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  entries = lj_trace_test_root_entry_successes();
  assert(call_loop(L, "string_byte_literal_trace", "string_byte_value",
                   &value) == LUA_OK && value == 98.0*80.0);
  if (lj_trace_test_root_entry_successes() == entries)
    fprintf(stderr, "literal string-byte had no native root: publishes=%u "
            "cleanups=%u aborts=%u error=%u exits=%u\n",
            lj_trace_test_root_entry_publishes(),
            lj_trace_test_root_entry_cleanups(),
            lj_trace_test_abort_count(),
            (unsigned)lj_trace_test_last_abort_error(),
            lj_trace_test_exit_calls());
  assert(lj_trace_test_root_entry_successes() > entries);
  {
    GCtrace *literal_owner;
    lua_getglobal(L, "string_byte_literal_trace");
    literal_no = proto_trace_acq(funcproto(funcV(L->top-1)));
    lua_pop(L, 1);
    literal_owner = traceref_safe(L2J(L), literal_no);
    assert(literal_no != 0 && literal_owner &&
           trace_runnable_acq(literal_owner, literal_no) &&
           bc_op(trace_startins_acq(literal_owner)) == BC_FORL);
  }
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

  /* Dynamic-index admission proves conversion, same-string length, guarded
  ** subtraction, nonnegative offset and exact one-byte width independently. */
  arm64_fixture_init_trace(&f, L, "string_byte_dynamic_trace_clone",
                           dynamic_no);
  f.view.proto = trace_startpt_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  {
    IRRef conv = 0, upper = 0, lenref = 0, addov = 0, nonneg = 0;
    IRRef subov = 0, width = 0, dynamic_strref = 0, one = 0;
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      int32_t k;
      if (!conv && f.ir[r].o == IR_CONV && f.ir[r].t.irt == IRT_INT &&
          f.ir[r].op2 == (IRCONV_INT_NUM|IRCONV_ANY)) conv = r;
      if (conv && !addov && f.ir[r].o == IR_ADDOV &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == conv &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == -1)
        addov = r;
      if (conv && !upper && f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          ((f.ir[r].o == IR_ULE && f.ir[r].op1 == conv &&
            f.ir[r].op2 >= REF_FIRST && f.ir[f.ir[r].op2].o == IR_FLOAD &&
            f.ir[f.ir[r].op2].op2 == IRFL_STR_LEN) ||
           (f.ir[r].o == IR_UGE && f.ir[r].op2 == conv &&
            f.ir[r].op1 >= REF_FIRST && f.ir[f.ir[r].op1].o == IR_FLOAD &&
            f.ir[f.ir[r].op1].op2 == IRFL_STR_LEN))) upper = r;
      if (addov && !nonneg && f.ir[r].o == IR_GE &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == addov &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 0)
        nonneg = r;
      if (addov && !subov && f.ir[r].o == IR_SUBOV &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == conv &&
          f.ir[r].op2 == addov)
        subov = r;
      if (subov && !width && f.ir[r].o == IR_EQ &&
          f.ir[r].t.irt == (IRT_INT|IRT_GUARD) && f.ir[r].op1 == subov &&
          kint(f.ir, f.semantic.nk, f.ir[r].op2, &k) && k == 1)
        width = r, one = f.ir[r].op2;
      if (addov && f.ir[r].o == IR_STRREF && f.ir[r].op2 == addov)
        dynamic_strref = r;
    }
    assert(conv && upper && addov && nonneg && subov && width &&
           dynamic_strref && f.ir[subov].op1 == conv &&
           f.ir[subov].op2 == addov && f.ir[nonneg].op1 == addov &&
           f.ir[dynamic_strref].op2 == addov);
    assert(one);
    lenref = f.ir[upper].o == IR_ULE ? f.ir[upper].op2 : f.ir[upper].op1;
    /* The complete proof must bind both operands of the guarded range
    ** subtraction, independently in semantic IR and post-RA IR. */
    assert(lj_asm_arm64_test_string_byte_dynamic_index_ir(&f.semantic, conv));
    assert(lj_asm_arm64_test_string_byte_dynamic_index_postra(&f.view, conv));
    {
      IRRef saved_semantic = f.ir[subov].op2;
      IRRef saved_postra = f.postra[subov].op2;
      f.ir[subov].op2 = lenref;
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_ir(&f.semantic,
                                                              conv));
      f.ir[subov].op2 = saved_semantic;
      f.postra[subov].op2 = lenref;
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_postra(&f.view,
                                                                  conv));
      f.postra[subov].op2 = saved_postra;
      assert(lj_asm_arm64_test_string_byte_dynamic_index_ir(&f.semantic,
                                                             conv));
      assert(lj_asm_arm64_test_string_byte_dynamic_index_postra(&f.view,
                                                                 conv));
    }
    {
      LJArm64PostRAView wrong_view = f.view;
      wrong_view.proto = wrong_proto;
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_postra(
          &wrong_view, conv));
      assert(lj_asm_arm64_test_string_byte_dynamic_index_postra(
          &f.view, conv));
    }
    {
      GCtrace fake_semantic = f.semantic;
      LJArm64PostRAView fake_view = f.view;
      IRIns unrelated;
      IRIns *fake_ir, *fake_postra;
      IRRef at = conv;
      memset(&unrelated, 0, sizeof(unrelated));
      unrelated.o = IR_CONV;
      unrelated.t.irt = IRT_INT;
      unrelated.op1 = f.ir[conv].op1;
      unrelated.op2 = IRCONV_INT_NUM|IRCONV_ANY;
      fake_ir = insert_irref(f.ir, f.semantic.nins, at, unrelated);
      fake_postra = insert_irref(f.postra, f.view.nins, at, unrelated);
      fake_semantic.ir = fake_ir;
      fake_semantic.nins++;
      fake_view.ir = fake_postra;
      fake_view.nins++;
      /* An unrelated ANY conversion before a real byte graph must not borrow
      ** the graph's proof. The original conversion remains at conv+1. */
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_ir(&fake_semantic, at));
      assert(!lj_asm_arm64_test_string_byte_dynamic_index_postra(&fake_view, at));
      assert(lj_asm_arm64_test_string_byte_dynamic_index_ir(&fake_semantic,
                                                            at+1u));
      assert(lj_asm_arm64_test_string_byte_dynamic_index_postra(&fake_view,
                                                                at+1u));
      free(fake_postra); free(fake_ir);
    }
    {
      GCtrace fake_semantic = f.semantic;
      LJArm64PostRAView fake_view = f.view;
      IRIns unrelated;
      IRIns *fake_ir, *fake_postra;
      IRRef at = f.ir[dynamic_strref].op2;
      memset(&unrelated, 0, sizeof(unrelated));
      unrelated.o = IR_ULE;
      unrelated.t.irt = IRT_INT|IRT_GUARD;
      unrelated.op1 = f.ir[conv].op1; /* Numeric source, not converted index. */
      unrelated.op2 = lenref;
      fake_ir = insert_irref(f.ir, f.semantic.nins, at, unrelated);
      fake_postra = insert_irref(f.postra, f.view.nins, at, unrelated);
      fake_semantic.ir = fake_ir;
      fake_semantic.nins++;
      fake_view.ir = fake_postra;
      fake_view.nins++;
      /* The valid upper guard still certifies the STRREF. An unrelated
      ** unsigned guard beside it cannot borrow that bound. */
      assert(lj_asm_arm64_test_string_byte_dynamic_guard_ir(&fake_semantic,
                                                             upper));
      assert(lj_asm_arm64_test_string_byte_dynamic_guard_postra(&fake_view,
                                                                 upper));
      assert(!lj_asm_arm64_test_string_byte_dynamic_guard_ir(&fake_semantic,
                                                             at));
      assert(!lj_asm_arm64_test_string_byte_dynamic_guard_postra(&fake_view,
                                                                 at));
      fake_ir[at].op2 = 1;
      fake_postra[at].op2 = 1;
      assert(!lj_asm_arm64_test_string_byte_dynamic_guard_ir(&fake_semantic,
                                                             at));
      assert(!lj_asm_arm64_test_string_byte_dynamic_guard_postra(&fake_view,
                                                                 at));
      free(fake_postra); free(fake_ir);
    }
#define REJECT_DYNAMIC(change) do { \
  int sem_ok, post_ok; \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; sem_ok = semantic(&f); post_ok = allocated(&f); \
  if (sem_ok || post_ok) { \
    fprintf(stderr, "dynamic string-byte mutation accepted at line %d " \
                    "(semantic=%d postRA=%d)\\n", __LINE__, sem_ok, post_ok); \
    abort(); \
  } \
} while (0)
#define REJECT_DYNAMIC_POSTRA(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; assert(semantic(&f)); \
  if (allocated(&f)) { \
    fprintf(stderr, "dynamic string-byte postRA mutation accepted at line %d\\n", \
            __LINE__); \
    abort(); \
  } \
} while (0)
    REJECT_DYNAMIC(f.ir[conv].op2 = IRCONV_INT_NUM|IRCONV_CHECK;
                   f.postra[conv].op2 = IRCONV_INT_NUM|IRCONV_CHECK);
    REJECT_DYNAMIC(f.ir[conv].op1 = f.ir[dynamic_strref].op1;
                   f.postra[conv].op1 = f.postra[dynamic_strref].op1);
    REJECT_DYNAMIC(f.ir[upper].op2 = f.ir[upper].op1;
                   f.postra[upper].op2 = f.postra[upper].op1);
    REJECT_DYNAMIC(f.ir[lenref].op1 = f.ir[conv].op1;
                   f.postra[lenref].op1 = f.postra[conv].op1);
    REJECT_DYNAMIC(f.ir[nonneg].op1 = conv;
                   f.postra[nonneg].op1 = conv);
    REJECT_DYNAMIC(f.ir[addov].o = IR_ADD;
                   f.postra[addov].o = IR_ADD);
    REJECT_DYNAMIC(f.ir[addov].op2 = f.ir[addov].op1;
                   f.postra[addov].op2 = f.postra[addov].op1);
    REJECT_DYNAMIC(f.ir[subov].op1 = f.ir[conv].op1;
                   f.postra[subov].op1 = f.postra[conv].op1);
    REJECT_DYNAMIC(f.ir[one].i = 2; f.postra[one].i = 2);
    REJECT_DYNAMIC(f.ir[dynamic_strref].op2 = conv;
                   f.postra[dynamic_strref].op2 = conv);
    REJECT_DYNAMIC(f.ir[dynamic_strref].op1 = f.ir[conv].op1;
                   f.postra[dynamic_strref].op1 = f.postra[conv].op1);
    REJECT_DYNAMIC_POSTRA(f.postra[conv].r = RID_D0);
    REJECT_DYNAMIC_POSTRA(f.view.proto = wrong_proto);
#undef REJECT_DYNAMIC
#undef REJECT_DYNAMIC_POSTRA
  }
  arm64_fixture_free(&f);

  /* Constant folding removes the byte load but can leave an unreferenced
  ** KKPTR for the literal's interior address in the constant pool. */
  arm64_fixture_init_trace(&f, L, "string_byte_literal_trace_clone", literal_no);
  assert(semantic(&f) && allocated(&f));
  {
    IRRef kkp = 0, payload, add = 0, identity = 0, rename = 0;
    unsigned nkkp = 0;
    IRRef k;
    SnapNo sn;
    for (k = f.semantic.nk; k < REF_TRUE; ) {
      IROp op = (IROp)f.ir[k].o;
      if (op == IR_KKPTR) {
        assert(f.ir[k].t.irt == IRT_PGC && f.ir[k].op12 == 0);
        kkp = k;
        nkkp++;
        k += 1u + LJ_GC64; /* Paired payload is not an IR header. */
      } else if (op == IR_KGC) {
        assert(k + LJ_GC64 < REF_TRUE);
        k += 1u + LJ_GC64;
      } else if (op == IR_KNUM) {
        assert(k + 1u < REF_TRUE);
        k += 2u;
      } else {
        k++;
      }
    }
    assert(kkp && nkkp == 1);
    payload = kkp + LJ_GC64;
    assert(payload < REF_TRUE);
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      if (f.ir[r].o == IR_ADDOV &&
          ((f.ir[r].op1 == kkp) || (f.ir[r].op2 == kkp)))
        add = r;
      if (f.ir[r].o == IR_EQ && f.ir[r].t.irt == (IRT_FUNC|IRT_GUARD))
        identity = r;
    }
    assert(!add && identity);
    for (sn = 0; sn < f.view.nsnap; sn++) {
      const SnapShot *snap = &f.snaps[sn];
      MSize i;
      for (i = 0; i < snap_nent_acq(snap); i++)
        assert(snap_ref(f.map[snap_mapofs_acq(snap)+i]) != kkp);
    }

    /* The dead constant is not a pointer capability: making it an IR or
    ** snapshot value must be rejected by both independent validators. */
    for (r = REF_FIRST; r < f.semantic_nins; r++)
      if (f.ir[r].o == IR_ADDOV) { add = r; break; }
    assert(add);
    arm64_fixture_reset(&f);
    f.ir[add].op2 = kkp;
    f.postra[add].op2 = kkp;
    assert(!semantic(&f) && !allocated(&f));

    /* The paired GC64 payload is data, not a constant value either. */
    arm64_fixture_reset(&f);
    f.ir[add].op2 = payload;
    f.postra[add].op2 = payload;
    assert(!semantic(&f) && !allocated(&f));

    arm64_fixture_reset(&f);
    {
      SnapEntry old;
      MSize mapidx = 0;
      for (sn = 0; sn < f.view.nsnap; sn++) {
        if (snap_nent_acq(&f.snaps[sn]) != 0) {
          mapidx = snap_mapofs_acq(&f.snaps[sn]);
          break;
        }
      }
      assert(sn < f.view.nsnap);
      old = f.map[mapidx];
      f.map[mapidx] = snap_setref(old, kkp);
      f.postmap[mapidx] = snap_setref(old, kkp);
      assert(!semantic(&f) && !allocated(&f));
      f.map[mapidx] = old;
      f.postmap[mapidx] = old;

      f.map[mapidx] = snap_setref(old, payload);
      f.postmap[mapidx] = snap_setref(old, payload);
      assert(!semantic(&f) && !allocated(&f));
      f.map[mapidx] = old;
      f.postmap[mapidx] = old;
    }

    /* Allocator tail renames are absent from the semantic view but must not
    ** be able to make the paired payload an allocated value. */
    for (r = f.semantic_nins; r < f.nins; r++)
      if (f.postra[r].o == IR_RENAME) { rename = r; break; }
    assert(rename);
    arm64_fixture_reset(&f);
    f.postra[rename].op1 = payload;
    assert(semantic(&f) && !allocated(&f));

    /* A different fast function cannot inherit byte's folded constant. */
    arm64_fixture_reset(&f);
    f.ir[identity].op2 = REF_TRUE;
    f.postra[identity].op2 = REF_TRUE;
    assert(!semantic(&f) && !allocated(&f));
  }
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  test_negative_constant_variant(L, "negative_one", ",-1",
      "string_byte_negative_constant_value");
  test_negative_constant_variant(L, "same_end", ",-2,-2",
      "string_byte_negative_constant_value");
  test_negative_constant_variant(L, "nil_end", ",-2,nil",
      "string_byte_negative_constant_value");
  test_fixed_byte_call(L,"",0,1);
  test_fixed_byte_call(L,",2,2",1,2);
  test_fixed_byte_call(L,",2,4",1,4);
  test_fixed_byte_call(L,",2,nil",1,2);
  test_fixed_byte_call(L,",2,last",1,2);
  test_dynamic_range_byte(L,2,0);
  test_dynamic_range_byte(L,4,0);
  test_dynamic_range_byte(L,4.9,1);
  test_u8_snapshot(L);
  test_integer_dynamic_index(L,0);
  test_integer_dynamic_index(L,1);
  test_dynamic_range_byte(L,-2,0);
  test_dynamic_range_byte(L,-2.9,1);
  test_dynamic_start_end_byte(L,0,0);
  test_dynamic_start_end_byte(L,0,1);
  test_dynamic_start_end_byte(L,1,0);
  test_dynamic_start_end_byte(L,1,1);
  for (unsigned width=2;width<=16;width*=2) {
    test_fixed_multiple_bytes(L,width,1,0);
    test_fixed_multiple_bytes(L,width,2,0);
  }
  test_fixed_multiple_bytes(L,3,1,0);
  for (unsigned mode=1;mode<=6;mode++) {
    for (unsigned width=2;width<=16;width*=2) {
      test_fixed_multiple_bytes(L,width,1,mode);
      test_fixed_multiple_bytes(L,width,2,mode);
    }
  }
  for (unsigned mode=7;mode<=10;mode++) {
    static const unsigned widths[]={2,3,4,8,16};
    for (unsigned wi=0;wi<sizeof(widths)/sizeof(widths[0]);wi++) {
      test_fixed_multiple_bytes(L,widths[wi],1,mode);
      test_fixed_multiple_bytes(L,widths[wi],2,mode);
    }
  }
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
