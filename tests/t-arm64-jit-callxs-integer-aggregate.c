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
static GCproto *aggregate_candidate_pt;

static int set_aggregate_candidate(lua_State *L)
{
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  aggregate_candidate_pt = funcproto(funcV(L->top-1));
  return 0;
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

static GCtrace *find_aggregate_trace(jit_State *J, GCproto *pt)
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
	irt_type(ir_load_acq(&trace_ir_acq(T)[ref]).t) == IRT_U64;
    }
    if (calls != 0 && saves >= calls && loads != 0) return T;
  }
  return NULL;
}

static IRRef find_integer_aggregate_load(const IRIns *ir, IRRef nins)
{
  IRRef ref;
  for (ref = REF_BASE; ref < nins; ref++)
    if (ir_load_acq(&ir[ref]).o == IR_XLOAD &&
	irt_type(ir_load_acq(&ir[ref]).t) == IRT_U64) return ref;
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
    assert(ir[offset].i == (int32_t)sizeof(GCcdata));
    ir[offset].i++;
  } else {
	assert(ir[offset].o == IR_KINT64 && ir[offset].t.irt == IRT_I64 &&
	       ir[offset+1u].tv.u64 ==
	   sizeof(GCcdata));
    ir[offset+1u].tv.u64++;
  }
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
  assert(!lj_ccall_arm64_signature(ctype_ctsG(G(L)),
				   global_cdata_ctype(L, name), &abi));
}

static void test_aggregate_signatures(lua_State *L)
{
  assert_aggregate_signature(L, "__arm64_int_pair_fn", 8);
  assert_aggregate_signature(L, "__arm64_byte_fn", 1);
  assert_aggregate_signature(L, "__arm64_half_fn", 2);
  assert_aggregate_signature(L, "__arm64_word_fn", 4);
  assert_unsupported_aggregate_signature(L, "__arm64_float_pair_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_nested_pair_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_mixed_union_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_big_pair_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_bitfield_fn");
  assert_unsupported_aggregate_signature(L, "__arm64_pair_no_gpr_fn");
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

static void test_live_aggregate_admission(lua_State *L, GCtrace *T,
						 const BCIns *meta_pc)
{
  Arm64TraceFixture fx;
  jit_State *J;
  IRRef nins, nk, semantic_nins, load, addr, offset, call = 0, typeguard;
  IRRef ktrace = 0, ref;
  IRIns *copy;
  LJArm64PostRAView postra;
  IRRef postra_nins = 0;
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
	"__arm64_integer_aggregate_clone", T->traceno, meta_pc);
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
  load = find_integer_aggregate_load(fx.good, nins);
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
  typeguard = find_cdata_type_guard(copy, nk, postra_nins, load);
  assert(fx.J->loopref != 0 && ktrace != 0 && typeguard != 0);

  J->curfinal = T;
  J->parent = 0;
  J->exitno = 0;
  J->framedepth = J->retdepth = 0;
  J->ktrace = (IRRef1)ktrace;
  assert(semantic_accepts(J, &fx.semantic, copy, nins, semantic_nins));

  copy_trace_ir(copy, fx.good, nk, nins);
  copy[load].t.irt = IRT_U32;
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
  fx.postra[load].r = RID_D0;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[load].s = SPS_FIRST-1u;
  assert(!lj_asm_arm64_postra_admit(&postra, &semantic_nins));
  copy_trace_ir(fx.postra, trace_ir_acq(T), nk, nins);
  postra = fx.view;
  postra.ir = fx.postra;
  fx.postra[load].t.irt = IRT_U32;
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
  lua_pushcfunction(L, get_aggregate_native_width_count);
  lua_setglobal(L, "_aggregate_native_width_count");
  if (luaL_dofile(L, argv[1]) != 0) {
    const char *error = lua_tostring(L, -1);
    fprintf(stderr, "integer aggregate Lua fixture failed: %s\n",
	    error ? error : "(non-string error)");
    assert(0);
  }
  assert(native_width_finishes[1] && native_width_finishes[2] &&
	 native_width_finishes[4] && native_width_finishes[8]);
  test_aggregate_signatures(L);
  {
    GCproto *pt = global_proto(L, "__arm64_integer_aggregate_candidate");
    jit_State *J = G2J(G(L));
    GCtrace *T;
    assert(pt == aggregate_candidate_pt);
    T = find_aggregate_trace(J, pt);
    assert(T != NULL);
    meta_pc = aggregate_meta_pc(T);
    assert(meta_pc != NULL);
    test_live_aggregate_admission(L, T, meta_pc);
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
