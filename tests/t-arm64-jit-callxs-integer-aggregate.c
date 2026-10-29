/* Native entry evidence for fixed one-GPR integer aggregate CALLXS. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_ccall.h"
#include "lj_ccall_arm64.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ff.h"
#include "lj_obj.h"
#include "lj_tg.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/arm64_trace_fixture.h"
#include "lib/lua_fixture_helpers.h"

static TGState *expected_tg;
static unsigned native_finishes;
static unsigned native_width_finishes[9];
static unsigned native_result_finishes[9];
static GCproto *aggregate_candidate_pt;
static GCproto *result_candidate_pt[9];
static GCproto *active_result_candidate_pt;
static GCproto *hfa_candidate_pt;
static unsigned hfa_native_finishes;
static GCproto *active_hfa_result_candidate_pt;
static GCproto *hfa_result_candidate_pt[65];
static unsigned hfa_result_native_finishes[65];

static int fixture_error_handler(lua_State *L)
{
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  return 1;
}

static int set_aggregate_candidate(lua_State *L)
{
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  aggregate_candidate_pt = funcproto(funcV(L->top-1));
  return 0;
}

static int set_hfa_candidate(lua_State *L)
{
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  hfa_candidate_pt = funcproto(funcV(L->top-1));
  return 0;
}

static int set_result_candidate(lua_State *L)
{
  int width = (int)luaL_checkinteger(L, 1);
  assert(width == 1 || width == 2 || width == 4 || width == 8);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  active_result_candidate_pt = funcproto(funcV(L->top-1));
  result_candidate_pt[width] = active_result_candidate_pt;
  return 0;
}

static int get_result_native_count(lua_State *L)
{
  int width = (int)luaL_checkinteger(L, 1);
  assert(width == 1 || width == 2 || width == 4 || width == 8);
  lua_pushinteger(L, (lua_Integer)native_result_finishes[width]);
  return 1;
}

static int set_hfa_result_candidate(lua_State *L)
{
  int width = (int)luaL_checkinteger(L, 1);
	assert(width == 4 || width == 8 || width == 12 || width == 16 ||
	       width == 24 || width == 32 || width == 40 || width == 64);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  active_hfa_result_candidate_pt = funcproto(funcV(L->top-1));
  hfa_result_candidate_pt[width] = active_hfa_result_candidate_pt;
  return 0;
}

static int get_hfa_result_native_count(lua_State *L)
{
  int width = (int)luaL_checkinteger(L, 1);
	assert(width == 4 || width == 8 || width == 12 || width == 16 ||
	       width == 24 || width == 32 || width == 40 || width == 64);
  lua_pushinteger(L, (lua_Integer)hfa_result_native_finishes[width]);
  return 1;
}

static int get_hfa_native_count(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)hfa_native_finishes);
  return 1;
}

static int begin_hfa_witness(lua_State *L)
{
  UNUSED(L);
  hfa_native_finishes = 0;
  return 0;
}

static int inject_callx_abi_failure(lua_State *L)
{
  int stage = (int)luaL_checkinteger(L, 1);
  assert(stage >= 0 && stage <= 2);
  lua_pushboolean(L, lj_asm_arm64_test_callx_abi_failure((uint32_t)stage));
  return 1;
}

static int get_hfa_trace_count(lua_State *L)
{
  jit_State *J = G2J(G(L));
  TraceNo no;
  unsigned count = 0;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    IRRef ref;
    unsigned members = 0, calls = 0;
    if (T == NULL || !trace_runnable_acq(T, no) ||
	trace_root_acq(T) != 0 || trace_startpt_acq(T) != hfa_candidate_pt)
      continue;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      members += ins.o == IR_XLOAD &&
	(irt_type(ins.t) == IRT_FLOAT || irt_type(ins.t) == IRT_NUM);
      calls += ins.o == IR_CALLXS;
    }
    if (members != 0 && calls) count++;
  }
  lua_pushinteger(L, (lua_Integer)count);
  return 1;
}

static int get_aggregate_native_width_count(lua_State *L)
{
  int width = (int)luaL_checkinteger(L, 1);
  assert(width == 1 || width == 2 || width == 4 || width == 8);
  lua_pushinteger(L, (lua_Integer)native_width_finishes[width]);
  return 1;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
}

static GCtrace *find_aggregate_trace(jit_State *J, GCproto *pt,
				     IRType load_type)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    IRRef ref;
    unsigned calls = 0, saves = 0;
    unsigned loads = 0;
    if (T == NULL || !trace_runnable_acq(T, no) ||
	trace_root_acq(T) != 0 || trace_startpt_acq(T) != pt ||
	bc_op(trace_startins_acq(T)) != BC_FORL) continue;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IROp op = ir_load_acq(&trace_ir_acq(T)[ref]).o;
      calls += op == IR_CALLXS;
      saves += op == IR_XSAVE;
      loads += op == IR_XLOAD &&
	irt_type(ir_load_acq(&trace_ir_acq(T)[ref]).t) == load_type;
    }
    if (calls != 0 && saves >= calls && loads != 0) return T;
  }
  return NULL;
}

static GCtrace *find_aggregate_trace_min_load_count(jit_State *J, GCproto *pt,
						   IRType load_type,
						   unsigned min_loads)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    IRRef ref;
    unsigned calls = 0, saves = 0, loads = 0;
    if (T == NULL || !trace_runnable_acq(T, no) ||
	trace_root_acq(T) != 0 || trace_startpt_acq(T) != pt ||
	bc_op(trace_startins_acq(T)) != BC_FORL) continue;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      calls += ins.o == IR_CALLXS;
      saves += ins.o == IR_XSAVE;
      loads += ins.o == IR_XLOAD && irt_type(ins.t) == load_type;
    }
    if (calls != 0 && saves >= calls && loads >= min_loads) return T;
  }
  return NULL;
}

static IRRef find_aggregate_load_index(const IRIns *ir, IRRef nins,
				       IRType type, unsigned index)
{
  IRRef ref;
  for (ref = REF_BASE; ref < nins; ref++)
    if (ir_load_acq(&ir[ref]).o == IR_XLOAD &&
	irt_type(ir_load_acq(&ir[ref]).t) == type && index-- == 0) return ref;
  return 0;
}

static IRRef next_constant(const IRIns *ir, IRRef ref)
{
  IROp op = ir_load_acq(&ir[ref]).o;
  return (op == IR_KNUM || op == IR_KINT64 ||
	(LJ_GC64 && (op == IR_KGC || op == IR_KPTR || op == IR_KKPTR))) ?
	ref+2u : ref+1u;
}

static int aggregate_constant_member(const IRIns *ir, IRRef nk, IRRef target)
{
  IRRef ref;
  if (target < nk || target >= REF_TRUE) return 0;
  for (ref = nk; ref < REF_TRUE; ) {
    IRIns constant = ir_load_acq(&ir[ref]);
    IRRef next;
    if (ref == target) {
      IRType type = irt_type(constant.t);
      if ((constant.o == IR_KNUM || constant.o == IR_KINT64 ||
	   (LJ_GC64 && (constant.o == IR_KGC || constant.o == IR_KPTR ||
			constant.o == IR_KKPTR))) && ref+1u >= REF_TRUE)
	return 0;
      switch ((IROp)constant.o) {
	case IR_KNUM: return type == IRT_NUM || type == IRT_P64;
	case IR_KINT: return type == IRT_INT || type == IRT_I8 ||
			 type == IRT_U8 || type == IRT_I16 ||
			 type == IRT_U16 || type == IRT_U32;
	case IR_KINT64: return type == IRT_I64 || type == IRT_U64 ||
			   type == IRT_P64;
	case IR_KGC: return type == IRT_PGC || type == IRT_P64 ||
		       type == IRT_STR || type == IRT_FUNC || type == IRT_TAB ||
		       type == IRT_CDATA || type == IRT_UDATA ||
		       type == IRT_PROTO || type == IRT_THREAD;
	case IR_KPTR: case IR_KKPTR:
	  return type == IRT_P32 || type == IRT_P64 ||
		 type == IRT_LIGHTUD || type == IRT_STR ||
		 type == IRT_FUNC || type == IRT_TAB;
	case IR_KPRI: return type == IRT_FALSE || type == IRT_TRUE ||
			 type == IRT_NIL;
	case IR_KNULL: return type == IRT_NIL || type == IRT_CDATA;
	default: return 0;
      }
    }
    next = (constant.o == IR_KNUM || constant.o == IR_KINT64 ||
	    (LJ_GC64 && (constant.o == IR_KGC || constant.o == IR_KPTR ||
			 constant.o == IR_KKPTR))) ?
	   (ref+1u < REF_TRUE ? ref+2u : REF_DROP) : ref+1u;
    if (next <= ref || next > REF_TRUE) return 0;
    ref = next;
  }
  return 0;
}

static void copy_trace_ir(IRIns *dst, const IRIns *src, IRRef nk, IRRef nins)
{
  assert(nk <= nins);
  memcpy(dst+nk, src+nk, (size_t)(nins-nk)*sizeof(IRIns));
}

static void mutate_aggregate_offset(IRIns *ir, IRRef offset)
{
  if (ir[offset].o == IR_KINT) {
    ir[offset].i++;
  } else {
	assert(ir[offset].o == IR_KINT64 && ir[offset].t.irt == IRT_I64);
    ir[offset+1u].tv.u64++;
  }
}

static void swap_callxs_values(IRIns *ir, IRRef call, IRRef lhs, IRRef rhs)
{
  IRRef ref;
  unsigned found_lhs = 0, found_rhs = 0;
  ref = ir[call].op1;
  while (ref >= REF_FIRST && ref < call && ir[ref].o == IR_CARG) {
    IRRef previous = ir[ref].op1;
    if (ir[ref].op2 == lhs) {
      ir[ref].op2 = rhs;
      found_lhs++;
    } else if (ir[ref].op2 == rhs) {
      ir[ref].op2 = lhs;
      found_rhs++;
    }
    ref = previous;
  }
  assert(found_lhs == 1 && found_rhs == 1);
}

/* Test-side CARG walk: recover source-order values without calling the
** production semantic/post-RA decoder. */
static unsigned test_callxs_values(const IRIns *ir, IRRef call,
				   IRRef *values, unsigned capacity)
{
  IRRef reversed[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir[call].op1;
  unsigned n = 0, i;
  if (ref == REF_NIL) return 0;
  for (;;) {
    if (n == capacity || n == LJ_ARM64_ABI_MAX_ARGS || ref >= call)
      return 0;
    if (ir[ref].o != IR_CARG) {
      reversed[n++] = ref;
      break;
    }
    reversed[n++] = ir[ref].op2;
    ref = ir[ref].op1;
  }
  for (i = 0; i < n; i++) values[i] = reversed[n-i-1u];
  return n;
}

static IRRef find_cdata_type_guard(const IRIns *ir, IRRef nk, IRRef nins,
				   IRRef load)
{
  IRRef address = ir_load_acq(&ir[load]).op1;
  IRRef source = ir_load_acq(&ir[address]).op1;
  IRRef ref;
  for (ref = source+1u; ref < load; ref++) {
    IRIns field = ir_load_acq(&ir[ref]);
    IRRef guard;
    if (field.o != IR_FLOAD || field.op1 != source ||
	field.op2 != IRFL_CDATA_CTYPEID) continue;
    for (guard = ref+1u; guard < load; guard++) {
      IRIns eq = ir_load_acq(&ir[guard]);
      IRRef k = eq.op1 == ref ? eq.op2 : eq.op2 == ref ? eq.op1 : 0;
      IRRef constant;
      if (eq.o != IR_EQ || k < nk || k >= REF_TRUE) continue;
      for (constant = nk; constant < REF_TRUE;
	   constant = next_constant(ir, constant)) {
	if (constant == k && ir_load_acq(&ir[constant]).o == IR_KINT)
	  return guard;
      }
    }
  }
  (void)nins;
  return 0;
}

static CTypeID global_cdata_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static void assert_aggregate_signature(lua_State *L, const char *name,
				       unsigned size)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 2 && abi.result_class == LJ_ARM64_ABI_GPR &&
	 abi.arg[0].kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE &&
	 abi.arg[0].cls == LJ_ARM64_ABI_GPR && abi.arg[0].reg == 0 &&
	 abi.arg[0].size == size && abi.arg[1].kind == LJ_ARM64_ABI_KIND_INTEGER &&
	 abi.arg[1].size == 4 && abi.arg[1].reg == 1 && !abi.stack_size);
}

static void assert_unsupported_aggregate_signature(lua_State *L,
						   const char *name)
{
  LJArm64ABISignature abi;
  if (lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				       global_cdata_ctype(L, name), &abi)) {
    fprintf(stderr, "unexpectedly accepted unsupported ABI signature: %s\n",
	    name);
    abort();
  }
}

static void assert_integer_stack_signature(lua_State *L, const char *name,
                                            unsigned trailing, unsigned width)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
         global_cdata_ctype(L,name),&abi));
  assert(abi.nargs==9u+trailing && abi.nparams==abi.nargs && abi.ngpr==8u &&
         !abi.nfpr && abi.stack_size==8u+4u*trailing);
  for (i=0;i<8u;i++)
    assert(abi.arg[i].reg==i && abi.arg[i].size==4u &&
           abi.arg[i].kind==LJ_ARM64_ABI_KIND_INTEGER);
  assert(abi.arg[8].reg==LJ_ARM64_ABI_STACK && abi.arg[8].stack_offset==0 &&
         abi.arg[8].size==width &&
         abi.arg[8].kind==LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE);
  if (trailing)
    assert(abi.arg[9].reg==LJ_ARM64_ABI_STACK && abi.arg[9].stack_offset==8 &&
           abi.arg[9].size==4 && abi.arg[9].kind==LJ_ARM64_ABI_KIND_INTEGER);
}

static void assert_padded_integer_stack_signature(lua_State *L,
                                                 const char *name,
                                                 unsigned width)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
    global_cdata_ctype(L,name), &abi));
  assert(abi.nargs==11 && abi.nparams==11 && abi.ngpr==8 &&
         abi.nfpr==0 && abi.stack_size==20);
  for (i=0;i<8;i++)
    assert(abi.arg[i].reg==i && abi.arg[i].size==4);
  assert(abi.arg[8].reg==LJ_ARM64_ABI_STACK && abi.arg[8].size==1 &&
         abi.arg[8].stack_offset==0 &&
         abi.arg[8].kind==LJ_ARM64_ABI_KIND_INTEGER);
  assert(abi.arg[9].reg==LJ_ARM64_ABI_STACK && abi.arg[9].size==width &&
         abi.arg[9].stack_offset==8 &&
         abi.arg[9].kind==LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE);
  assert(abi.arg[10].reg==LJ_ARM64_ABI_STACK && abi.arg[10].size==4 &&
         abi.arg[10].stack_offset==16 &&
         abi.arg[10].kind==LJ_ARM64_ABI_KIND_INTEGER);
}

static void assert_float_hfa_signature(lua_State *L, const char *name)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 2 && abi.nparams == 1 &&
	 abi.result_class == LJ_ARM64_ABI_GPR &&
	 abi.arg[0].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.arg[0].source_index == 0 && abi.arg[0].member_index == 0 &&
	 abi.arg[0].member_count == 2 && abi.arg[0].reg == 0 &&
	 abi.arg[1].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.arg[1].source_index == 0 && abi.arg[1].member_index == 1 &&
	 abi.arg[1].member_count == 2 && abi.arg[1].reg == 1 &&
	 !abi.stack_size);
}

static void assert_float_hfa_salt_signature(lua_State *L, const char *name)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 3 && abi.nparams == 2 &&
	 abi.result_class == LJ_ARM64_ABI_FPR &&
	 abi.arg[0].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.arg[0].source_index == 0 && abi.arg[0].member_index == 0 &&
	 abi.arg[0].member_count == 2 && abi.arg[0].reg == 0 &&
	 abi.arg[1].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.arg[1].source_index == 0 && abi.arg[1].member_index == 1 &&
	 abi.arg[1].member_count == 2 && abi.arg[1].reg == 1 &&
	 abi.arg[2].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[2].source_index == 1 && abi.arg[2].reg == 2 &&
	 !abi.stack_size);
}

static void assert_float_hfa_multilane_salt_signature(lua_State *L,
						       const char *name,
						       unsigned member_size,
						       unsigned count)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == count+1 && abi.nparams == 2 &&
	 abi.result_class == LJ_ARM64_ABI_FPR &&
	 abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT);
  for (i = 0; i < count; i++)
    assert(abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	   abi.arg[i].source_index == 0 && abi.arg[i].member_index == i &&
	   abi.arg[i].member_count == count &&
	   abi.arg[i].size == member_size && abi.arg[i].reg == i);
  assert(abi.arg[count].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[count].source_index == 1 && abi.arg[count].reg == count &&
	 !abi.stack_size);
}

static void assert_float_hfa_plain_multilane_signature(lua_State *L,
						       const char *name,
						       unsigned member_size,
						       unsigned count)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == count && abi.nparams == 1 &&
	 abi.result_class == LJ_ARM64_ABI_GPR &&
	 abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER);
  for (i = 0; i < count; i++)
    assert(abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	   abi.arg[i].source_index == 0 && abi.arg[i].member_index == i &&
	   abi.arg[i].member_count == count &&
	   abi.arg[i].size == member_size && abi.arg[i].reg == i);
  assert(!abi.stack_size);
}

static void assert_float_hfa_stack_signature(lua_State *L, const char *name,
                                               unsigned width, unsigned count)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 9u+count && abi.nparams == 10 && abi.nfpr == 8 &&
	 abi.stack_size == width*count+8u && abi.result_class == LJ_ARM64_ABI_FPR);
  for (i = 0; i < 8; i++)
    assert(abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	   abi.arg[i].cls == LJ_ARM64_ABI_FPR && abi.arg[i].reg == i);
  for (i = 0; i < count; i++)
    assert(abi.arg[8+i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	   abi.arg[8+i].cls == LJ_ARM64_ABI_FPR &&
	   abi.arg[8+i].reg == LJ_ARM64_ABI_STACK &&
	   abi.arg[8+i].member_index == i && abi.arg[8+i].size == width &&
	   abi.arg[8+i].member_count == count &&
	   abi.arg[8+i].stack_offset == i*width);
  assert(abi.arg[8+count].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[8+count].cls == LJ_ARM64_ABI_FPR &&
	 abi.arg[8+count].reg == LJ_ARM64_ABI_STACK &&
	 abi.arg[8+count].stack_offset == width*count);
}

static void assert_partial_hfa_stack_signature(lua_State *L, const char *name,
					       unsigned width, unsigned count)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 8u+count && abi.nparams == 9 && abi.nfpr == 8 &&
	 abi.stack_size == width*count+8u &&
	 abi.result_class == LJ_ARM64_ABI_FPR);
  for (i = 0; i < 7; i++)
    assert(abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	   abi.arg[i].cls == LJ_ARM64_ABI_FPR && abi.arg[i].reg == i);
  for (i = 0; i < count; i++)
    assert(abi.arg[7+i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	   abi.arg[7+i].cls == LJ_ARM64_ABI_FPR &&
	   abi.arg[7+i].reg == LJ_ARM64_ABI_STACK &&
	   abi.arg[7+i].source_index == 7 &&
	   abi.arg[7+i].member_index == i &&
	   abi.arg[7+i].member_count == count &&
	   abi.arg[7+i].size == width &&
	   abi.arg[7+i].stack_offset == i*width);
  assert(abi.arg[7+count].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[7+count].cls == LJ_ARM64_ABI_FPR &&
	 abi.arg[7+count].reg == LJ_ARM64_ABI_STACK &&
	 abi.arg[7+count].source_index == 8 &&
	 abi.arg[7+count].stack_offset == width*count);
}

static void assert_hfa_reference_pointer_signature(lua_State *L,
						    const char *name)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 2 && abi.nparams == 2 &&
	 abi.arg[0].kind == LJ_ARM64_ABI_KIND_POINTER &&
	 abi.arg[0].size == sizeof(void *) && abi.arg[0].reg == 0 &&
	 abi.arg[1].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[1].reg == 0);
}

static void assert_one_hfa_signature(lua_State *L, const char *name,
				     unsigned member_size)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.nargs == 2 && abi.nparams == 2 &&
	 abi.result_class == LJ_ARM64_ABI_FPR &&
	 abi.arg[0].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.arg[0].source_index == 0 && abi.arg[0].member_index == 0 &&
	 abi.arg[0].member_count == 1 && abi.arg[0].size == member_size &&
	 abi.arg[0].reg == 0 &&
	 abi.arg[1].kind == LJ_ARM64_ABI_KIND_FLOAT &&
	 abi.arg[1].source_index == 1 && abi.arg[1].reg == 1 &&
	 !abi.stack_size);
}

static void assert_vararg_scalar_signature(lua_State *L, const char *name)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.is_vararg && abi.nparams == 1 && abi.nargs == 1 &&
	 abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER &&
	 abi.result_size == sizeof(int32_t));
}

static void assert_integer_aggregate_result_signature(lua_State *L,
						     const char *name,
						     unsigned size)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.result_class == LJ_ARM64_ABI_GPR &&
	 abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE &&
	 abi.result_size == size && abi.result_unsigned &&
	 abi.result_declared_id != 0 && abi.result_id != 0);
}

static void assert_hfa_result_signature(lua_State *L, const char *name,
					unsigned member_size, unsigned count)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				 global_cdata_ctype(L, name), &abi));
  assert(abi.result_class == LJ_ARM64_ABI_FPR &&
	 abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
	 abi.result_hfa_count == count && abi.result_size == member_size &&
	 abi.result_declared_id != 0 && abi.result_id != 0);
  for (i = 0; i < count; i++)
    assert(abi.result_hfa_offset[i] == i*member_size);
}

static void test_aggregate_signatures(lua_State *L)
{
  assert_aggregate_signature(L, "__arm64_int_pair_fn", 8);
  assert_aggregate_signature(L, "__arm64_byte_fn", 1);
  assert_aggregate_signature(L, "__arm64_half_fn", 2);
  assert_aggregate_signature(L, "__arm64_word_fn", 4);
  assert_float_hfa_signature(L, "__arm64_float_pair_fn");
  assert_float_hfa_salt_signature(L, "__arm64_float_pair_double_salt_fn");
  assert_float_hfa_multilane_salt_signature(L,
	 "__arm64_float_triple_double_salt_fn", sizeof(float), 3);
  assert_float_hfa_multilane_salt_signature(L,
	 "__arm64_double_quad_double_salt_fn", sizeof(double), 4);
  assert_unsupported_aggregate_signature(L, "__arm64_double_quint_arg_fn");
  assert_unsupported_aggregate_signature(L,
	 "__arm64_aligned_double_triple_arg_fn");
  assert_hfa_reference_pointer_signature(L,
	 "__arm64_float_triple_ref_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_hfa_fpr_overflow_fn");
  assert_one_hfa_signature(L, "__arm64_float_one_double_salt_fn",
			   sizeof(float));
  assert_one_hfa_signature(L, "__arm64_double_one_double_salt_fn",
			   sizeof(double));
  assert_vararg_scalar_signature(L, "__arm64_vararg_i32_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_float_vararg_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_mixed_float_hfa_fn");
  assert_float_hfa_plain_multilane_signature(L,
	 "__arm64_double_pair_fn", sizeof(double), 2);
  assert_float_hfa_plain_multilane_signature(L,
	 "__arm64_float_triple_fn", sizeof(float), 3);
  assert_float_hfa_stack_signature(L, "__arm64_float_pair_stack_fn", 4, 2);
  assert_float_hfa_stack_signature(L, "__arm64_wide_stack_fn_1", 4, 4);
  assert_float_hfa_stack_signature(L, "__arm64_wide_stack_fn_2", 8, 2);
  assert_float_hfa_stack_signature(L, "__arm64_wide_stack_fn_3", 8, 3);
  assert_float_hfa_stack_signature(L, "__arm64_wide_stack_fn_4", 8, 4);
  assert_float_hfa_stack_signature(L, "__arm64_wide_stack_fn_5", 8, 1);
  assert_partial_hfa_stack_signature(L, "__arm64_partial_hfa_stack_fn", 4, 2);
  assert_partial_hfa_stack_signature(L,
	 "__arm64_partial_float_quad_stack_fn", 4, 4);
  assert_unsupported_aggregate_signature(L, "__arm64_float_triple_stack_fn");
  assert_unsupported_aggregate_signature(L,
	 "__arm64_partial_hfa_stack_unsupported_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_nested_pair_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_mixed_union_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_big_pair_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_bitfield_fn");
  assert_integer_stack_signature(L, "__arm64_pair_no_gpr_fn", 0, 8);
  assert_integer_stack_signature(L, "__arm64_pair_stack_fn", 1, 8);
  assert_integer_stack_signature(L, "__arm64_word_stack_fn", 0, 4);
  assert_padded_integer_stack_signature(L, "__arm64_small_stack_fn_1", 1);
  assert_padded_integer_stack_signature(L, "__arm64_small_stack_fn_2", 2);
  assert_padded_integer_stack_signature(L, "__arm64_small_stack_fn_3", 4);
  assert_unsupported_aggregate_signature(L, "__arm64_pair_stack_vararg_fn");
  assert_integer_aggregate_result_signature(L, "__arm64_return_byte_fn", 1);
  assert_integer_aggregate_result_signature(L, "__arm64_return_half_fn", 2);
  assert_integer_aggregate_result_signature(L, "__arm64_return_word_fn", 4);
  assert_integer_aggregate_result_signature(L, "__arm64_return_u64_fn", 8);
  assert_hfa_result_signature(L, "__arm64_return_float_hfa_fn", 4, 1);
  assert_hfa_result_signature(L, "__arm64_return_double_hfa_fn", 8, 1);
  assert_hfa_result_signature(L, "__arm64_return_float_pair_hfa_fn", 4, 2);
  assert_hfa_result_signature(L, "__arm64_return_double_pair_hfa_fn", 8, 2);
  assert_hfa_result_signature(L, "__arm64_return_float_triple_hfa_fn", 4, 3);
  assert_hfa_result_signature(L, "__arm64_return_float_quad_hfa_fn", 4, 4);
  assert_hfa_result_signature(L, "__arm64_return_double_triple_hfa_fn", 8, 3);
  assert_hfa_result_signature(L, "__arm64_return_double_quad_hfa_fn", 8, 4);
  assert_unsupported_aggregate_signature(L, "__arm64_double_quint_result_fn");
  assert_unsupported_aggregate_signature(L,
      "__arm64_aligned_double_triple_result_fn");
  assert_unsupported_aggregate_signature(L,
      "__arm64_aligned_float_pair_result_fn");
  assert_hfa_result_signature(L, "__arm64_double_pair_result_fn", 8, 2);
  assert_unsupported_aggregate_signature(L, "__arm64_aligned_float_result_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_padded_float_result_fn");
  {
    LJArm64ABISignature abi;
    assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
	global_cdata_ctype(L, "__arm64_word_result_alias_fn"), &abi));
    assert(abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE &&
	   abi.result_declared_id != 0 && abi.result_id != 0 &&
	   abi.result_size == 4);
    assert(!lj_ccall_arm64_signature(ctype_ctsG(G(L)),
	global_cdata_ctype(L, "__arm64_aligned_result_fn"), &abi));
  }
}

static const BCIns *aggregate_meta_pc(const GCtrace *T)
{
  IRRef ref;
  for (ref = T->nk; ref < REF_TRUE; ) {
    IRIns constant;
    GCobj *obj;
    const BCIns *pc;
    IRRef next;
    if (!aggregate_constant_member(T->ir, T->nk, ref)) return NULL;
    constant = ir_load_acq(&T->ir[ref]);
    next = (constant.o == IR_KNUM || constant.o == IR_KINT64 ||
	    (LJ_GC64 && (constant.o == IR_KGC || constant.o == IR_KPTR ||
			 constant.o == IR_KKPTR))) ?
	   (ref+1u < REF_TRUE ? ref+2u : REF_DROP) : ref+1u;
    if (constant.o == IR_KGC && constant.t.irt == IRT_FUNC) {
	obj = ir_kgc_load_acq(&T->ir[ref]);
	if (obj && checkptrGC(obj) &&
	    obj->gch.gct == (uint32_t)~LJ_TFUNC &&
	    lj_func_ffid_acq(gco2func(obj)) == FF_ffi_meta___call) {
	  pc = mref_acq(gco2func(obj)->c.pc, const BCIns);
	  if (!pc || ((uintptr_t)(const void *)pc & (sizeof(BCIns)-1u)) != 0 ||
	      bc_op((BCIns)la_load32_acq((const uint32_t *)pc)) != BC_FUNCC)
	    return NULL;
	  return pc;
	}
    }
    if (next <= ref || next > REF_TRUE) return NULL;
    ref = next;
  }
  return NULL;
}

static int semantic_accepts(jit_State *J, GCtrace *source,
			    IRIns *copy, IRRef nins, IRRef semantic_nins)
{
  GCtrace view = *source;
  LJArm64IRReject reject;
  int ok;
  view.ir = copy;
  view.nins = semantic_nins;
  (void)nins;
  ok = lj_asm_arm64_ir_admit(J, &view, &reject);
  return ok;
}

static void test_live_aggregate_admission_at(lua_State *L, GCtrace *T,
						     const BCIns *meta_pc,
						     const char *clone_name,
						     IRType load_type,
						     unsigned load_index)
{
  Arm64TraceFixture fx;
  jit_State *J;
  IRRef nins, nk, semantic_nins, load, addr, offset, call = 0, typeguard;
  IRRef ktrace = 0, ref;
  IRRef argvalues[LJ_ARM64_ABI_MAX_ARGS];
  unsigned argcount = 0;
  unsigned const_pairs[4], nconst_pairs = 0;
  IRIns *copy;
  IRRef stack_hfa_first = 0;
  LJArm64PostRAView postra;
  IRRef postra_nins = 0;
  MSize raw_slot = 0;
  SnapNo snapno;
  SnapEntry saved_entry;
  GG_State *fakegg;
  jit_State *fixture_jit;
  {
    const SnapShot *snaps = trace_snap_acq(T);
    const SnapEntry *map = trace_snapmap_acq(T);
    GCproto *origin = trace_startpt_acq(T);
    SnapNo s;
    for (s = 0; s < trace_nsnap_acq(T); s++) {
      uint64_t footer;
      const BCIns *pc;
      uintptr_t pcaddr, lo, hi;
      memcpy(&footer, &map[snaps[s].mapofs+snaps[s].nent], sizeof(footer));
      pc = (const BCIns *)(uintptr_t)(footer >> 8);
      pcaddr = (uintptr_t)(const void *)pc;
      lo = (uintptr_t)(const void *)proto_bc(origin);
      hi = lo+(uintptr_t)origin->sizebc*sizeof(BCIns);
      if (pcaddr < lo || pcaddr >= hi) {
	assert(pcaddr == (uintptr_t)(const void *)meta_pc);
      }
    }
  }
  arm64_fixture_init_trace_callxs_metapc(&fx, L,
	clone_name, T->traceno, meta_pc);
  (void)arm64_fixture_init;
  arm64_fixture_reset(&fx);
  fx.view.cts = ctype_ctsG(G(L));
  fx.view.proto = trace_startpt_acq(T);
  fx.view.startpc = trace_startpc_acq(T);
  /* The semantic CALLXS proof obtains CTState through J2G(J), so keep the
  ** synthetic recorder state at the normal embedded-J offset. */
  fixture_jit = fx.J;
  fakegg = (GG_State *)calloc(1, sizeof(GG_State));
  assert(fakegg != NULL);
  fakegg->J = *fixture_jit;
  setmref(fakegg->g.ctype_state, ctype_ctsG(G(L)));
  free(fixture_jit);
  fx.J = &fakegg->J;
  J = fx.J;
  nins = fx.nins;
  nk = T->nk;
  semantic_nins = fx.semantic_nins;
  copy = fx.ir;
	load = find_aggregate_load_index(fx.good, nins, load_type, load_index);
  assert(load != 0 && nk > 0);
  postra = fx.view;
  assert(lj_asm_arm64_postra_admit(&postra, &postra_nins));
  assert(postra_nins > REF_BASE);
  for (ref = nk; ref < REF_TRUE; ref = next_constant(copy, ref)) {
    IRIns constant = ir_load_acq(&copy[ref]);
    if (constant.o == IR_KGC && constant.t.irt == IRT_P64 &&
	ref+1u < REF_TRUE && ir_kgc_load_acq(&copy[ref]) == obj2gco(T))
      ktrace = ref;
  }
  addr = ir_load_acq(&copy[load]).op1;
  offset = ir_load_acq(&copy[addr]).op2;
  assert(offset >= nk && offset < REF_TRUE);
  if (copy[offset].o == IR_KINT64) assert(offset+1u < REF_TRUE);
  mutate_aggregate_offset(copy, offset);
  /* Restore the positive source before running the other independent probes. */
  copy_trace_ir(copy, fx.good, nk, nins);
  for (ref = REF_BASE; ref < semantic_nins; ref++)
    if (copy[ref].o == IR_CALLXS) { call = ref; break; }
  assert(call != 0);
  if (strcmp(clone_name, "__arm64_narrow_constant_clone") == 0) {
    argcount = test_callxs_values(copy, call, argvalues,
				  LJ_ARM64_ABI_MAX_ARGS);
    assert(argcount == 18);
    assert(copy[argvalues[8]].o == IR_CONV &&
	   copy[argvalues[9]].o == IR_CONV &&
	   copy[argvalues[10]].o == IR_CONV &&
	   copy[argvalues[11]].o == IR_CONV &&
	   argvalues[8] != argvalues[9] && argvalues[10] != argvalues[11]);
    const_pairs[0] = 8; const_pairs[1] = 9;
    const_pairs[2] = 10; const_pairs[3] = 11;
    nconst_pairs = 2;
  } else if (strcmp(clone_name, "__arm64_small_constant_clone") == 0) {
    argcount = test_callxs_values(copy, call, argvalues,
				  LJ_ARM64_ABI_MAX_ARGS);
    assert(argcount == 11 && argvalues[8] != argvalues[10]);
    const_pairs[0] = 8; const_pairs[1] = 10;
    nconst_pairs = 1;
  }
  if ((load_type == IRT_FLOAT || load_type == IRT_NUM) && load_index != 0)
    stack_hfa_first = find_aggregate_load_index(fx.good, nins, load_type,
						       load_index-1u);
  typeguard = find_cdata_type_guard(copy, nk, postra_nins, load);
  assert(fx.J->loopref != 0 && ktrace != 0 && typeguard != 0);

  J->curfinal = T;
  J->parent = 0;
  J->exitno = 0;
  J->framedepth = J->retdepth = 0;
  J->ktrace = (IRRef1)ktrace;
  assert(semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  if (nconst_pairs != 0) {
    unsigned pair;
    for (pair = 0; pair < nconst_pairs; pair++) {
      unsigned lhs = const_pairs[2u*pair], rhs = const_pairs[2u*pair+1u];
      copy_trace_ir(copy, fx.good, nk, nins);
      swap_callxs_values(copy, call, argvalues[lhs], argvalues[rhs]);
      assert(!semantic_accepts(J, &fx.semantic, copy, nins,
			       semantic_nins));
      copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
      postra = fx.view;
      postra.ir = fx.postra;
      swap_callxs_values(fx.postra, call, argvalues[lhs], argvalues[rhs]);
      assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
    }
  }
  for (snapno = 0; snapno < fx.semantic.nsnap; snapno++)
    if (snap_nent_acq(&fx.snaps[snapno]) != 0) {
      raw_slot = snap_mapofs_acq(&fx.snaps[snapno]);
      break;
    }
  assert(snapno < fx.semantic.nsnap);
  saved_entry = fx.map[raw_slot];
  fx.map[raw_slot] = snap_setref(saved_entry, load);
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  fx.map[raw_slot] = saved_entry;

  if (stack_hfa_first != 0) {
    copy_trace_ir(copy, fx.good, nk, nins);
    swap_callxs_values(copy, call, stack_hfa_first, load);
    assert(!semantic_accepts(J, &fx.semantic, copy, nins,
			     semantic_nins));
  }

  copy_trace_ir(copy, fx.good, nk, nins);
  copy[load].t.irt = load_type == IRT_U32 ? IRT_U64 : IRT_U32;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[addr].op1 = REF_BASE;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[typeguard].op1 = REF_TRUE;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  mutate_aggregate_offset(copy, offset);
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[call].op1 = load;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));

  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  saved_entry = fx.postmap[raw_slot];
  fx.postmap[raw_slot] = snap_setref(saved_entry, load);
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  fx.postmap[raw_slot] = saved_entry;
  if (stack_hfa_first != 0) {
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    swap_callxs_values(fx.postra, call, stack_hfa_first, load);
    assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  }
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
	fx.postra[load].r = (load_type == IRT_FLOAT || load_type == IRT_NUM) ?
	  RID_X0 : RID_D0;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[load].s = SPS_FIRST-1u;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[load].t.irt = load_type == IRT_U32 ? IRT_U64 : IRT_U32;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  mutate_aggregate_offset(fx.postra, offset);
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  fx.postra[call].op1 = load;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));

  fx.J = NULL;  /* J is embedded in fakegg; free the enclosing allocation. */
  arm64_fixture_free(&fx);
  free(fakegg);
  lua_pop(L, 1);
}

static void test_live_aggregate_admission(lua_State *L, GCtrace *T,
						 const BCIns *meta_pc,
						 const char *clone_name,
						 IRType load_type)
{
  test_live_aggregate_admission_at(L, T, meta_pc, clone_name, load_type, 0);
}

static GCtrace *find_integer_result_trace(jit_State *J, GCproto *pt,
					 IRType type)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    IRRef ref;
    int call = 0, box = 0, store = 0;
    if (T == NULL || !trace_runnable_acq(T, no) ||
	trace_root_acq(T) != 0 || trace_startpt_acq(T) != pt ||
	bc_op(trace_startins_acq(T)) != BC_FORL) continue;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      call |= ins.o == IR_CALLXS && irt_type(ins.t) == type;
      box |= ins.o == IR_CNEW;
      store |= ins.o == IR_XSTORE;
    }
    if (call && box && store) return T;
  }
  return NULL;
}

static void test_live_integer_result_admission(lua_State *L, GCtrace *T,
						 const BCIns *meta_pc,
						 const char *clone_name,
						 IRType type)
{
  Arm64TraceFixture fx;
  jit_State *J, *fixture_jit;
  GG_State *fakegg;
	IRRef ref, box = 0, call = 0, first_call = 0, store = 0;
	IRRef hiop = 0, storehi = 0, hiop2 = 0, storehi2 = 0;
	IRRef hiop3 = 0, storehi3 = 0;
  IRRef ktrace = 0, badctype = 0, wrong_offset = 0;
  IRRef nk = T->nk, nins = T->nins, semantic_nins;
  IRIns *copy;
  LJArm64PostRAView postra;
  IRRef postra_nins = 0;
  SnapNo snapno;
  MSize slot = 0;
  SnapEntry saved;
  assert(T != NULL);
  arm64_fixture_init_trace_callxs_metapc(&fx, L, clone_name,
	T->traceno, meta_pc);
  arm64_fixture_reset(&fx);
  fx.view.cts = ctype_ctsG(G(L));
  fx.view.proto = trace_startpt_acq(T);
  fx.view.startpc = trace_startpc_acq(T);
  fixture_jit = fx.J;
  fakegg = (GG_State *)calloc(1, sizeof(GG_State));
  assert(fakegg != NULL);
  fakegg->J = *fixture_jit;
  setmref(fakegg->g.ctype_state, ctype_ctsG(G(L)));
  free(fixture_jit);
  fx.J = &fakegg->J;
  J = fx.J;
  semantic_nins = fx.semantic_nins;
  copy = fx.ir;
  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&copy[ref]);
    if (ins.o == IR_CNEW) box = ref;
	  if (ins.o == IR_CALLXS && irt_type(ins.t) == type) {
	    if (!first_call) first_call = ref;
	    call = ref;
	  }
    if (ins.o == IR_HIOP && call && ref == call+1u) hiop = ref;
    if (ins.o == IR_HIOP && call && ref == call+2u) hiop2 = ref;
    if (ins.o == IR_HIOP && call && ref == call+3u) hiop3 = ref;
  }
  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&copy[ref]);
    if (ins.o == IR_XSTORE && ins.op2 == call) store = ref;
    if (ins.o == IR_XSTORE && hiop && ins.op2 == hiop) storehi = ref;
    if (ins.o == IR_XSTORE && hiop2 && ins.op2 == hiop2) storehi2 = ref;
    if (ins.o == IR_XSTORE && hiop3 && ins.op2 == hiop3) storehi3 = ref;
  }
	  assert(box && call && first_call && call != first_call && store);
  if (T->ir[call+1u].o == IR_HIOP)
    assert(hiop == call+1u && storehi != 0);
  if (T->ir[call+2u].o == IR_HIOP)
    assert(hiop2 == call+2u && storehi2 != 0);
  if (T->ir[call+3u].o == IR_HIOP)
    assert(hiop3 == call+3u && storehi3 != 0);
  for (ref = nk; ref < REF_TRUE; ref = next_constant(copy, ref)) {
    IRIns constant = ir_load_acq(&copy[ref]);
    if (constant.o == IR_KGC && constant.t.irt == IRT_P64 &&
	ref+1u < REF_TRUE && ir_kgc_load_acq(&copy[ref]) == obj2gco(T))
      ktrace = ref;
    if (constant.o == IR_KINT && constant.t.irt == IRT_INT) {
      if (ref != copy[box].op1 && constant.i > 0 &&
          constant.i != copy[copy[box].op1].i)
        badctype = ref;
      if (constant.i == (int32_t)sizeof(GCcdata)) wrong_offset = ref;
    } else if (constant.o == IR_KINT64 && constant.t.irt == IRT_I64 &&
               ref+1u < REF_TRUE &&
               ir_load_acq(&copy[ref+1u]).tv.u64 == sizeof(GCcdata)) {
      wrong_offset = ref;
    }
  }
  assert(ktrace && badctype && fx.J->loopref != 0);
  if (hiop) assert(wrong_offset != 0);
  J->curfinal = T;
  J->parent = 0;
  J->exitno = 0;
  J->framedepth = J->retdepth = 0;
  J->ktrace = (IRRef1)ktrace;
  assert(semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  postra = fx.view;
  assert(lj_asm_arm64_postra_admit(&postra, &postra_nins));
  assert(postra_nins > REF_BASE);

  for (snapno = 0; snapno < fx.semantic.nsnap; snapno++) {
    MSize ofs = snap_mapofs_acq(&fx.snaps[snapno]);
    MSize nent = snap_nent_acq(&fx.snaps[snapno]), i;
    for (i = 0; i < nent; i++)
      if (snap_ref(snapentry_acq(&fx.map[ofs+i])) == box) {
	slot = ofs+i;
	break;
      }
    if (slot) break;
  }
  assert(slot);
  saved = fx.map[slot];
  fx.map[slot] = snap_setref(saved, call);
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  fx.map[slot] = saved;
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[store].t.irt = type == IRT_U8 ? IRT_U16 : IRT_U8;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[box].op1 = badctype;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[store].op2 = REF_BASE;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  copy_trace_ir(copy, fx.good, nk, nins);
  copy[store].op2 = first_call;
  assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  if (hiop) {
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[hiop].op1 = REF_BASE;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[hiop].op2 = REF_BASE;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[storehi].op2 = call;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[copy[storehi].op1].op2 = wrong_offset;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  }
  if (hiop2) {
    copy_trace_ir(copy, fx.good, nk, nins);
    saved = fx.map[slot];
    fx.map[slot] = snap_setref(saved, hiop2);
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    fx.map[slot] = saved;
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[hiop2].op2 = REF_BASE;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    if (hiop3) {
      copy_trace_ir(copy, fx.good, nk, nins);
      copy[hiop3].op2 = copy[hiop2].op2;
      assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    }
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[copy[storehi2].op1].op2 = wrong_offset;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[storehi2].op2 = hiop;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  }
  if (hiop3) {
    copy_trace_ir(copy, fx.good, nk, nins);
    copy[hiop2].op2 = fx.good[hiop3].op2;
    copy[hiop3].op2 = fx.good[hiop2].op2;
    assert(!semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));
  }

  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  assert(REF_FIRST < first_call);
  fx.postra[REF_FIRST].o = IR_HIOP;
  fx.postra[REF_FIRST].t.irt = IRT_FLOAT;
  fx.postra[REF_FIRST].op1 = 0;
  fx.postra[REF_FIRST].op2 = 0;
  fx.postra[REF_FIRST].r = RID_D0;
  assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));

  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  saved = fx.postmap[slot];
  fx.postmap[slot] = snap_setref(saved, call);
  assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  fx.postmap[slot] = saved;
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[store].t.irt = type == IRT_U8 ? IRT_U16 : IRT_U8;
  assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
	copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
	postra = fx.view;
	postra.ir = fx.postra;
	fx.postra[store].op2 = first_call;
	assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  if (hiop) {
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop].op1 = REF_BASE;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop].op2 = REF_BASE;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[storehi].op2 = call;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[fx.postra[storehi].op1].op2 = wrong_offset;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[call].r = RID_D1;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop].r = RID_D2;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  }
  if (hiop2) {
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    saved = fx.postmap[slot];
    fx.postmap[slot] = snap_setref(saved, hiop2);
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    fx.postmap[slot] = saved;
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop2].r = RID_D4;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop2].op2 = REF_BASE;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[storehi2].op2 = hiop;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  }
  if (hiop3) {
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop3].r = RID_D4;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    fx.postra[hiop3].op2 = fx.postra[hiop2].op2;
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
    copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
    postra = fx.view;
    postra.ir = fx.postra;
    {
      IRRef lane_tag = fx.postra[hiop2].op2;
      fx.postra[hiop2].op2 = fx.postra[hiop3].op2;
      fx.postra[hiop3].op2 = lane_tag;
    }
    assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  }
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[box].op1 = badctype;
  assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[store].op2 = REF_BASE;
  assert(!lj_asm_arm64_postra_admit(&postra, &postra_nins));

  fx.J = NULL;
  arm64_fixture_free(&fx);
  free(fakegg);
  lua_pop(L, 1);
}

static void observe_native_finish(TGState *tg)
{
  const LJFFINativeFrame *frame;
  GCtrace *T;
  IRRef ref;
  int has_call = 0;
  unsigned widths[9] = { 0 };
  assert(tg == expected_tg);
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1u);
  frame = &tg->ffi_native_frame[0];
  assert((lj_ffi_native_frame_flags_acq(frame) &
          (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
           LJ_FFI_NATIVE_FRAME_F_POSTCALL)) == LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert(lj_ffi_native_frame_L_acq(frame) != NULL);
  assert(lj_ffi_native_frame_trace_acq(frame) != NULL);
  assert(trace_native_pins_acq(lj_ffi_native_frame_trace_acq(frame)) != 0);
  T = lj_ffi_native_frame_trace_acq(frame);
  if (aggregate_candidate_pt != NULL && trace_root_acq(T) == 0 &&
      trace_startpt_acq(T) == aggregate_candidate_pt &&
      bc_op(trace_startins_acq(T)) == BC_FORL) {
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      if (ins.o == IR_CALLXS) has_call = 1;
      if (ins.o == IR_XLOAD) {
	switch (irt_type(ins.t)) {
	case IRT_U8: widths[1] = 1; break;
	case IRT_U16: widths[2] = 1; break;
	case IRT_U32: widths[4] = 1; break;
	case IRT_U64: widths[8] = 1; break;
	default: break;
	}
      }
    }
    if (has_call) {
      native_width_finishes[1] += widths[1];
      native_width_finishes[2] += widths[2];
      native_width_finishes[4] += widths[4];
      native_width_finishes[8] += widths[8];
    }
  }
  if (hfa_candidate_pt != NULL && trace_root_acq(T) == 0 &&
      trace_startpt_acq(T) == hfa_candidate_pt &&
      bc_op(trace_startins_acq(T)) == BC_FORL) {
    unsigned hfa_loads = 0;
    int callxs = 0;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      callxs |= ins.o == IR_CALLXS;
	  hfa_loads += ins.o == IR_XLOAD &&
	    (irt_type(ins.t) == IRT_FLOAT || irt_type(ins.t) == IRT_NUM);
    }
    if (callxs && hfa_loads != 0) hfa_native_finishes++;
  }
  if (active_result_candidate_pt != NULL && trace_root_acq(T) == 0 &&
      trace_startpt_acq(T) == active_result_candidate_pt &&
      bc_op(trace_startins_acq(T)) == BC_FORL) {
    int has_call = 0, has_box = 0, has_store = 0;
    IRType result_type = IRT_NIL;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      if (ins.o == IR_CALLXS) {
        has_call = 1;
        result_type = (IRType)irt_type(ins.t);
      } else if (ins.o == IR_CNEW) {
        has_box = 1;
      } else if (ins.o == IR_XSTORE) {
        has_store = 1;
      }
    }
    if (has_call && has_box && has_store) {
      switch (result_type) {
      case IRT_U8: native_result_finishes[1]++; break;
      case IRT_U16: native_result_finishes[2]++; break;
      case IRT_U32: native_result_finishes[4]++; break;
      case IRT_U64: native_result_finishes[8]++; break;
      default: break;
      }
    }
  }
  if (active_hfa_result_candidate_pt != NULL && trace_root_acq(T) == 0 &&
      trace_startpt_acq(T) == active_hfa_result_candidate_pt &&
      bc_op(trace_startins_acq(T)) == BC_FORL) {
    unsigned calls = 0, hiops = 0, boxes = 0, stores = 0;
    IRType result_type = IRT_NIL;
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
      IRIns ins = ir_load_acq(&trace_ir_acq(T)[ref]);
      if (ins.o == IR_CALLXS) {
        calls++;
        result_type = (IRType)irt_type(ins.t);
      } else if (ins.o == IR_CNEW) {
        boxes++;
      } else if (ins.o == IR_XSTORE) {
        stores++;
      } else if (ins.o == IR_HIOP) {
        hiops++;
      }
    }
    if (calls != 0 && boxes >= calls &&
        (result_type == IRT_FLOAT || result_type == IRT_NUM)) {
      if (hiops == calls && stores == 2*calls && result_type == IRT_FLOAT)
        hfa_result_native_finishes[12]++;
      else if (hiops == calls && stores == 2*calls && result_type == IRT_NUM)
        hfa_result_native_finishes[16]++;
      else if (hiops == 2*calls && stores == 3*calls &&
	       result_type == IRT_FLOAT)
	hfa_result_native_finishes[32]++;
      else if (hiops == 3*calls && stores == 4*calls &&
	       result_type == IRT_FLOAT)
	hfa_result_native_finishes[64]++;
      else if (hiops == 2*calls && stores == 3*calls &&
	       result_type == IRT_NUM)
	hfa_result_native_finishes[24]++;
      else if (hiops == 3*calls && stores == 4*calls &&
	       result_type == IRT_NUM)
	hfa_result_native_finishes[40]++;
      else if (hiops == 0 && stores == calls)
        hfa_result_native_finishes[result_type == IRT_FLOAT ? 4 : 8]++;
    }
  }
  native_finishes++;
}

int main(int argc, char **argv)
{
  lua_State *L;
  const BCIns *meta_pc;
  assert(argc == 2);
  L = ljt_lua_newstate_openlibs();
  expected_tg = L2TG(L);
  lj_ffi_native_trace_test_set_finish_hook(observe_native_finish);
  lua_pushcfunction(L, set_aggregate_candidate);
  lua_setglobal(L, "_set_aggregate_candidate");
  lua_pushcfunction(L, set_hfa_candidate);
  lua_setglobal(L, "_set_hfa_candidate");
  lua_pushcfunction(L, set_result_candidate);
  lua_setglobal(L, "_set_result_candidate");
  lua_pushcfunction(L, get_result_native_count);
  lua_setglobal(L, "_aggregate_result_native_count");
  lua_pushcfunction(L, set_hfa_result_candidate);
  lua_setglobal(L, "_set_hfa_result_candidate");
  lua_pushcfunction(L, get_hfa_result_native_count);
  lua_setglobal(L, "_hfa_result_native_count");
  lua_pushcfunction(L, get_hfa_native_count);
  lua_setglobal(L, "_hfa_native_count");
  lua_pushcfunction(L, begin_hfa_witness);
  lua_setglobal(L, "_begin_hfa_witness");
  lua_pushcfunction(L, inject_callx_abi_failure);
  lua_setglobal(L, "_inject_callx_abi_failure");
  lua_pushcfunction(L, get_hfa_trace_count);
  lua_setglobal(L, "_hfa_trace_count");
  lua_pushcfunction(L, get_aggregate_native_width_count);
  lua_setglobal(L, "_aggregate_native_width_count");
  lua_pushcfunction(L, fixture_error_handler);
  {
    int errfunc = lua_gettop(L);
    int status = luaL_loadfile(L, argv[1]);
    if (status == 0) status = lua_pcall(L, 0, 0, errfunc);
    if (status != 0) {
      const char *error = lua_tostring(L, -1);
    fprintf(stderr, "integer aggregate Lua fixture failed: %s\n",
	    error ? error : "(non-string error)");
      fprintf(stderr, "trace abort count=%u last=%u\n",
	      lj_trace_test_abort_count(),
	      (unsigned)lj_trace_test_last_abort_error());
      if (tvisint(&G2J(G(L))->errinfo))
        fprintf(stderr, "trace errinfo=%d\n",
	        (int32_t)intV(&G2J(G(L))->errinfo));
      assert(0);
    }
  }
  assert(native_width_finishes[1] && native_width_finishes[2] &&
	 native_width_finishes[4] && native_width_finishes[8]);
  test_aggregate_signatures(L);
  {
    static const unsigned widths[] = { 1, 2, 4, 8 };
    static const IRType types[] = { IRT_U8, IRT_U16, IRT_U32, IRT_U64 };
    unsigned i;
    jit_State *J = G2J(G(L));
    for (i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
      char name[64];
      GCproto *pt;
      GCtrace *T;
      snprintf(name, sizeof(name), "__arm64_integer_result_candidate_%u",
	       widths[i]);
      pt = global_proto(L, name);
      assert(pt == result_candidate_pt[widths[i]]);
      T = find_integer_result_trace(J, pt, types[i]);
      assert(T != NULL);
      meta_pc = aggregate_meta_pc(T);
      assert(meta_pc != NULL);
      test_live_integer_result_admission(L, T, meta_pc,
	"__arm64_integer_result_clone", types[i]);
    }
  }
  {
    static const unsigned widths[] = { 4, 8, 12, 16, 24, 32, 40, 64 };
    static const IRType types[] = { IRT_FLOAT, IRT_NUM, IRT_FLOAT, IRT_NUM,
				    IRT_NUM, IRT_FLOAT, IRT_NUM, IRT_FLOAT };
    unsigned i;
    jit_State *J = G2J(G(L));
    for (i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
      char name[64];
      GCproto *pt;
      GCtrace *T;
      snprintf(name, sizeof(name), "__arm64_hfa_result_candidate_%u",
	       widths[i]);
      pt = global_proto(L, name);
      assert(pt == hfa_result_candidate_pt[widths[i]]);
      T = find_integer_result_trace(J, pt, types[i]);
      assert(T != NULL);
      meta_pc = aggregate_meta_pc(T);
      assert(meta_pc != NULL);
      test_live_integer_result_admission(L, T, meta_pc,
	"__arm64_integer_result_clone", types[i]);
    }
  }
  {
    GCproto *pt = global_proto(L, "__arm64_float_hfa_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace(J, pt, IRT_FLOAT);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_float_hfa_clone", IRT_FLOAT);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_float_pair_stack_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace_min_load_count(J, pt, IRT_FLOAT, 2);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission_at(L, T, meta_pc,
	"__arm64_float_pair_stack_clone", IRT_FLOAT, 1);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_partial_hfa_stack_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace_min_load_count(J, pt, IRT_FLOAT, 2);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission_at(L, T, meta_pc,
	"__arm64_partial_hfa_stack_clone", IRT_FLOAT, 1);
  }
  for (unsigned i=1;i<=5;i++) {
    char candidate_name[64], clone_name[64];
    static const unsigned counts[]={4,2,3,4,1};
    IRType type=i==1?IRT_FLOAT:IRT_NUM;
    snprintf(candidate_name,sizeof(candidate_name),"__arm64_wide_stack_candidate_%u",i);
    snprintf(clone_name,sizeof(clone_name),"__arm64_wide_stack_clone_%u",i);
    GCproto *pt=global_proto(L,candidate_name);
    GCtrace *T=find_aggregate_trace_min_load_count(G2J(G(L)),pt,type,counts[i-1]);
    assert(T != NULL);
    meta_pc=aggregate_meta_pc(T); assert(meta_pc != NULL);
    test_live_aggregate_admission_at(L,T,meta_pc,clone_name,type,counts[i-1]-1u);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_integer_pair_stack_candidate");
    GCtrace *T = find_aggregate_trace(G2J(G(L)), pt, IRT_U64);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L,T,meta_pc,
      "__arm64_integer_pair_stack_clone",IRT_U64);
  }
  for (unsigned i=1;i<=3;i++) {
    char candidate_name[64], clone_name[64];
    IRType type = i==1 ? IRT_U8 : i==2 ? IRT_U16 : IRT_U32;
    GCproto *pt;
    GCtrace *T;
    snprintf(candidate_name,sizeof(candidate_name),"__arm64_small_stack_candidate_%u",i);
    snprintf(clone_name,sizeof(clone_name),"__arm64_small_stack_clone_%u",i);
    pt=global_proto(L,candidate_name);
    T=find_aggregate_trace(G2J(G(L)),pt,type);
    assert(T != NULL);
    meta_pc=aggregate_meta_pc(T); assert(meta_pc != NULL);
    test_live_aggregate_admission(L,T,meta_pc,clone_name,type);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_small_constant_candidate");
    GCtrace *T = find_aggregate_trace(G2J(G(L)), pt, IRT_U8);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_small_constant_clone", IRT_U8);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_narrow_constant_candidate");
    GCtrace *T = find_aggregate_trace(G2J(G(L)), pt, IRT_U8);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_narrow_constant_clone", IRT_U8);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_float_triple_hfa_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace_min_load_count(J, pt, IRT_FLOAT, 3);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission_at(L, T, meta_pc,
	"__arm64_float_hfa_clone", IRT_FLOAT, 2);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_double_quad_hfa_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace_min_load_count(J, pt, IRT_NUM, 4);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission_at(L, T, meta_pc,
	"__arm64_float_hfa_clone", IRT_NUM, 3);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_float_one_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace(J, pt, IRT_FLOAT);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_float_one_clone", IRT_FLOAT);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_double_one_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace(J, pt, IRT_NUM);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_double_one_clone", IRT_NUM);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_integer_callm_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T = find_aggregate_trace(J, pt, IRT_U64);
    IRRef ref;
    unsigned calls = 0;
    assert(T != NULL);
    for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
	if (ir_load_acq(&trace_ir_acq(T)[ref]).o == IR_CALLXS) calls++;
    assert(calls >= 2);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc,
	"__arm64_integer_callm_clone", IRT_U64);
  }
  {
    GCproto *pt = global_proto(L, "__arm64_integer_aggregate_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T;
    assert(pt == aggregate_candidate_pt);
    T = find_aggregate_trace(J, pt, IRT_U64);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
	 test_live_aggregate_admission(L, T, meta_pc,
	   "__arm64_integer_aggregate_clone", IRT_U64);
  }
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(native_finishes != 0);
  assert((lj_ffi_native_frame_sequence_acq(expected_tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(expected_tg) == 0);
  assert(lj_tg_in_native_acq(expected_tg) == 0);
  lua_close(L);
  return 0;
}

#else

int main(void)
{
  return 77;
}

#endif
