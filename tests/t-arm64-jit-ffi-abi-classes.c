/* Native Darwin ARM64 CALLXS ABI-class matrix and classifier negatives. */
#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
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
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT || !LJ_HASFFI || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED
#error "FFI ABI matrix requires experimental ARM64 CALLXS admission"
#endif

typedef void (*InstallObserver)(void (*fn)(unsigned));
typedef unsigned (*ReadCounter)(unsigned id);

static TGState *test_tg;
static unsigned native_seen[8];
static void *expected_func[8];

static void test_other_callsite_footer_rejected(lua_State *L)
{
  jit_State *J = G2J(G(L));
  GCfunc *fn;
  GCproto *pt;
  CTState *cts = ctype_ctsG(G(L));
  const BCIns *bc;
  TraceVec *tv;
  GCtrace *T = NULL;
  MSize trace_no, i, s, candidate = UINT_MAX, snapno = UINT_MAX;
  uintptr_t lo, hi;
  LJArm64PostRAView view = { 0 };
  SnapEntry *snapmap;
  IRIns *ircopy;
  uint64_t original, changed;
  IRRef ref;
  lua_getglobal(L, "__abi_run_three");
  assert(tvisfunc(L->top-1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  bc = proto_bc(pt);
  lo = (uintptr_t)(const void *)bc;
  hi = lo+(uintptr_t)pt->sizebc*sizeof(BCIns);
  tv = tracevec_acq(J);
  assert(tv != NULL);
  for (trace_no = 1; trace_no < tv->sizetrace; trace_no++) {
    GCtrace *trace = traceref_safe(J, trace_no);
    unsigned calls = 0;
    if (trace == NULL || trace_startpc_acq(trace) == NULL ||
	(uintptr_t)(const void *)trace_startpc_acq(trace) < lo ||
	(uintptr_t)(const void *)trace_startpc_acq(trace) >= hi)
      continue;
    for (ref = trace_nk_acq(trace); ref < trace_nins_acq(trace); ref++)
      if (ir_load_acq(&trace_ir_acq(trace)[ref]).o == IR_CALLXS)
	calls++;
    if (calls >= 3u) {
      T = trace;
      break;
    }
  }
  assert(T != NULL);
  view.ir = trace_ir_acq(T);
  view.owner = T;
  view.cts = cts;
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = bc;
  view.proto = pt;
  view.startpc = trace_startpc_acq(T);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.proto_numparams = pt->numparams;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.owner_private = 0;
  assert(lj_asm_arm64_postra_admit(&view, NULL));
  snapmap = (SnapEntry *)malloc(view.nsnapmap*sizeof(SnapEntry));
  assert(snapmap != NULL);
  memcpy(snapmap, view.snapmap, view.nsnapmap*sizeof(SnapEntry));
  view.snapmap = snapmap;
  for (s = 0; s < view.nsnap; s++) {
    const SnapShot *snap = &view.snap[s];
    IRRef at = snap_ref_acq(snap), scan;
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    SnapEntry footer[1+LJ_FR2];
    uint64_t pcbase;
    int before_leave = 0;
    if (ofs > view.nsnapmap || nent > view.nsnapmap-ofs ||
	view.nsnapmap-ofs-nent < 1u+LJ_FR2)
      continue;
    for (scan = at; scan < view.nins; scan++) {
      IRIns ins = ir_load_acq(&view.ir[scan]);
      if (scan > at && (ins.o == IR_XSAVE || ins.o == IR_CALLXS))
	break;
      if (ins.o == IR_CALLS &&
	  ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
	before_leave = 1;
	break;
      }
    }
    if (!before_leave) continue;
    for (i = 0; i < 1u+LJ_FR2; i++)
      footer[i] = snapentry_acq(&snapmap[ofs+nent+i]);
    memcpy(&pcbase, footer, sizeof(pcbase));
    if ((uintptr_t)(pcbase>>8) < lo || (uintptr_t)(pcbase>>8) >= hi)
      continue;
    snapno = s;
    original = pcbase;
    break;
  }
  assert(snapno != UINT_MAX);
  for (i = 0; i < pt->sizebc; i++) {
    if (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[i])) == BC_CALL &&
	lo+(i+1u)*sizeof(BCIns) !=
	(uintptr_t)(original>>8)) {
      candidate = i+1u;
      break;
    }
  }
  assert(candidate != UINT_MAX);
  changed = (uint64_t)(lo+(uintptr_t)candidate*sizeof(BCIns)) << 8;
  {
    const SnapShot *snap = &view.snap[snapno];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    memcpy(&((SnapEntry *)view.snapmap)[ofs+nent], &changed, sizeof(changed));
  }
  assert(!lj_asm_arm64_postra_admit(&view, NULL));
  {
    IRRef constref;
    int paired = 0, footer_count = 0;
    ircopy = (IRIns *)calloc(view.nins, sizeof(IRIns));
    assert(ircopy != NULL);
    memcpy(&ircopy[view.nk], &trace_ir_acq(T)[view.nk],
	   (view.nins-view.nk)*sizeof(IRIns));
    for (s = 0; s < view.nsnap; s++) {
      const SnapShot *snap = &view.snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      uint64_t pcbase;
      if (ofs > view.nsnapmap || nent > view.nsnapmap-ofs ||
	  view.nsnapmap-ofs-nent < 1u+LJ_FR2)
	continue;
      memcpy(&pcbase, &((SnapEntry *)view.snapmap)[ofs+nent],
	     sizeof(pcbase));
      if (pcbase == original) {
	memcpy(&((SnapEntry *)view.snapmap)[ofs+nent], &changed,
	       sizeof(changed));
	footer_count++;
      }
    }
    assert(footer_count > 0);
    for (constref = view.nk; constref < REF_TRUE; ) {
      IRIns k = ir_load_acq(&ircopy[constref]);
      IRRef next = constref+1u;
      if (k.o == IR_KNUM || k.o == IR_KINT64 ||
	  (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
		       k.o == IR_KKPTR)))
	next = constref+2u < REF_TRUE ? constref+2u : REF_DROP;
      if (next <= constref || next > REF_TRUE) break;
      if (k.o == IR_KNUM && constref+1u < REF_TRUE &&
	  ir_load_acq(&ircopy[constref+1u]).tv.u64 == original >> 8) {
	ircopy[constref+1u].tv.u64 = (uint64_t)(lo+
	    (uintptr_t)candidate*sizeof(BCIns));
	paired++;
      }
      constref = next;
    }
    assert(paired > 0);
    view.ir = ircopy;
    assert(!lj_asm_arm64_postra_admit(&view, NULL));
    free(ircopy);
  }
  free(snapmap);
}

static void observe_native_call(unsigned id)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  const LJFFINativeFrame *frame;
  GCtrace *trace;
  uint32_t flags;
  assert(id > 0 && id < 8);
  result = lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return;  /* Interpreter warm-up calls are counted separately by the library. */
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1);
  frame = &snapshot.frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert((flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
                   LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
                   LJ_FFI_NATIVE_FRAME_F_POSTCALL)) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_func_acq(frame) == expected_func[id]);
  assert(lj_ffi_native_frame_L_acq(frame) == lj_tg_load_cur_L(test_tg));
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  trace = lj_ffi_native_frame_trace_acq(frame);
  assert(trace != NULL && trace_native_pins_acq(trace) != 0);
  assert(lj_ffi_native_frame_trace_no_acq(frame) != 0);
  assert(lj_tg_in_native_acq(test_tg) == 1);
  assert(lj_tg_load_jit_base(test_tg) != NULL);
  assert(lj_tg_vmstate_load_acq(test_tg) ==
         (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  native_seen[id]++;
}

static CTypeID global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static void assert_abi_class(CTState *cts, CTypeID id, uint8_t result,
                             unsigned nargs, unsigned ngpr, unsigned nfpr)
{
  LJArm64ABISignature abi;
  assert(lj_ccall_arm64_signature(cts, id, &abi));
  assert(abi.result_class == result && abi.nargs == nargs);
  assert(abi.ngpr == ngpr && abi.nfpr == nfpr);
}

static void assert_rejected(CTState *cts, CTypeID id)
{
  LJArm64ABISignature abi;
  assert(!lj_ccall_arm64_signature(cts, id, &abi));
}

static void assert_argument_classes(CTState *cts, CTypeID id,
                                    const uint8_t *classes,
                                    const uint8_t *registers,
                                    unsigned nargs)
{
  LJArm64ABISignature abi;
  unsigned i;
  assert(lj_ccall_arm64_signature(cts, id, &abi) && abi.nargs == nargs);
  for (i = 0; i < nargs; i++) {
    assert(abi.arg[i].cls == classes[i]);
    assert(abi.arg[i].reg == registers[i]);
  }
}

static void test_long_callsite(lua_State *L)
{
	const size_t prefix_count = 33000;
	const char begin[] = "return function(fn) local x=0;";
	const char statement[] = "x=x+1;";
	const char end[] = "for i=1,300 do fn(9) end; return x end";
	size_t capacity = sizeof(begin)+prefix_count*(sizeof(statement)-1u)+
		  sizeof(end);
	char *source = (char *)malloc(capacity);
	char *out;
	size_t i;
	assert(source != NULL);
	out = source;
	memcpy(out, begin, sizeof(begin)-1u);
	out += sizeof(begin)-1u;
	for (i = 0; i < prefix_count; i++) {
		memcpy(out, statement, sizeof(statement)-1u);
		out += sizeof(statement)-1u;
	}
	memcpy(out, end, sizeof(end));
	assert(luaL_loadbuffer(L, source, (size_t)(out-source)+sizeof(end)-1u,
		"=long-callsite") == 0);
	free(source);
	assert(lua_pcall(L, 0, 1, 0) == 0);
	lua_setglobal(L, "__abi_long_callsite");
	ljt_lua_dostring(L,
		"assert(require('jit.util').funcinfo(__abi_long_callsite).bytecodes > 32768)\n"
		"assert(__abi_long_callsite(__abi_unused) == 33000)\n");
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  const char *path = getenv("LJ_ARM64_FFI_ABI_LIB");
  void *library;
  InstallObserver install;
  ReadCounter calls;
  CTState *cts;
  CTypeID six, mixed, many_a, many_b, many_c, unused, f32;
  CTypeID stack9, aggregate_arg, aggregate_result, vararg;
  unsigned i;
  assert(L != NULL && path != NULL);
  test_tg = L2TG(L);
  library = dlopen(path, RTLD_NOW|RTLD_LOCAL);
  assert(library != NULL);
  install = (InstallObserver)dlsym(library, "arm64_abi_install_observer");
  calls = (ReadCounter)dlsym(library, "arm64_abi_calls");
  assert(install != NULL && calls != NULL);
  expected_func[1] = dlsym(library, "arm64_abi_i32_six");
  expected_func[2] = dlsym(library, "arm64_abi_mixed");
  expected_func[3] = dlsym(library, "arm64_abi_many_a");
  expected_func[4] = dlsym(library, "arm64_abi_many_b");
  expected_func[5] = dlsym(library, "arm64_abi_many_c");
  expected_func[6] = dlsym(library, "arm64_abi_unused_result");
  expected_func[7] = dlsym(library, "arm64_abi_float_result");
  for (i = 1; i < 8; i++) assert(expected_func[i] != NULL);
  install(observe_native_call);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int32_t (*abi_six_fn)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "typedef double (*abi_mix_fn)(double,int32_t,double,int32_t,double);\n"
    "typedef int32_t (*abi_pair_fn)(int32_t,int32_t);\n"
    "typedef int32_t (*abi_unused_fn)(int32_t);\n"
    "typedef float (*abi_float_fn)(float,float);\n"
    "typedef struct { int64_t a,b; } abi_pair;\n"
    "int32_t arm64_abi_i32_six(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "double arm64_abi_mixed(double,int32_t,double,int32_t,double);\n"
    "int32_t arm64_abi_many_a(int32_t,int32_t);\n"
    "int32_t arm64_abi_many_b(int32_t,int32_t);\n"
    "int32_t arm64_abi_many_c(int32_t,int32_t);\n"
    "int32_t arm64_abi_unused_result(int32_t);\n"
    "float arm64_abi_float_result(float,float);\n"
    "int32_t arm64_abi_stack9(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);\n"
    "abi_pair arm64_abi_aggregate_arg(abi_pair);\n"
    "abi_pair arm64_abi_aggregate_result(int32_t);\n"
    "int32_t arm64_abi_vararg(int32_t,...);\n"
    "]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_ARM64_FFI_ABI_LIB')))\n"
    "_G.__abi_six = lib.arm64_abi_i32_six\n"
    "_G.__abi_mixed = lib.arm64_abi_mixed\n"
    "_G.__abi_a, _G.__abi_b, _G.__abi_c = lib.arm64_abi_many_a, lib.arm64_abi_many_b, lib.arm64_abi_many_c\n"
    "_G.__abi_unused = lib.arm64_abi_unused_result\n"
    "_G.__abi_f32 = lib.arm64_abi_float_result\n"
    "_G.__abi_stack9 = lib.arm64_abi_stack9\n"
    "_G.__abi_aggregate_arg = lib.arm64_abi_aggregate_arg\n"
    "_G.__abi_aggregate_result = lib.arm64_abi_aggregate_result\n"
    "_G.__abi_vararg = lib.arm64_abi_vararg\n"
    "function _G.__abi_run_six(fn,a,b,c,d,e,f,n)\n"
    " for i=1,n do if fn(a,b,c,d,e,f)~=91 then return false end end; return true end\n"
    "function _G.__abi_run_mixed(fn,a,b,c,d,e,n)\n"
    " for i=1,n do if fn(a,b,c,d,e)~=16 then return false end end; return true end\n"
    "function _G.__abi_run_three(fa,fb,fc,a,b,n)\n"
    " for i=1,n do fa(a,b); fb(a,b); fc(a,b) end; return true end\n"
    "function _G.__abi_run_unused(fn,a,n)\n"
    " for i=1,n do fn(a) end; return n end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  six = global_ctype(L, "__abi_six");
  mixed = global_ctype(L, "__abi_mixed");
  many_a = global_ctype(L, "__abi_a");
  many_b = global_ctype(L, "__abi_b");
  many_c = global_ctype(L, "__abi_c");
  unused = global_ctype(L, "__abi_unused");
  f32 = global_ctype(L, "__abi_f32");
  stack9 = global_ctype(L, "__abi_stack9");
  aggregate_arg = global_ctype(L, "__abi_aggregate_arg");
  aggregate_result = global_ctype(L, "__abi_aggregate_result");
  vararg = global_ctype(L, "__abi_vararg");

  assert_abi_class(cts, six, LJ_ARM64_ABI_GPR, 6, 6, 0);
  assert_abi_class(cts, mixed, LJ_ARM64_ABI_FPR, 5, 2, 3);
  assert_abi_class(cts, many_a, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, many_b, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, many_c, LJ_ARM64_ABI_GPR, 2, 2, 0);
  assert_abi_class(cts, unused, LJ_ARM64_ABI_GPR, 1, 1, 0);
  assert_abi_class(cts, f32, LJ_ARM64_ABI_FPR, 2, 0, 2);
  {
    const uint8_t gpr6[] = { 2, 2, 2, 2, 2, 2 };
    const uint8_t gpr_regs[] = { 0, 1, 2, 3, 4, 5 };
    const uint8_t mixed_classes[] = { 3, 2, 3, 2, 3 };
    const uint8_t mixed_regs[] = { 0, 0, 1, 1, 2 };
    assert_argument_classes(cts, six, gpr6, gpr_regs, 6);
    assert_argument_classes(cts, mixed, mixed_classes, mixed_regs, 5);
  }
  assert_rejected(cts, stack9);
  assert_rejected(cts, aggregate_arg);
  assert_rejected(cts, aggregate_result);
  assert_rejected(cts, vararg);
  assert_rejected(cts, UINT32_MAX);  /* malformed/out-of-range CType ID */

  ljt_lua_dostring(L,
    "assert(__abi_run_six(__abi_six,1,2,3,4,5,6,300))\n"
    "assert(__abi_run_mixed(__abi_mixed,1.25,2,3.5,4,5.25,300))\n"
    "assert(__abi_a(11,5)==16 and __abi_b(11,5)==6 and __abi_c(11,5)==14)\n"
    "assert(__abi_run_three(__abi_a,__abi_b,__abi_c,11,5,300))\n"
    "assert(__abi_run_unused(__abi_unused,9,300)==300)\n");
	{
	uint64_t before = calls(6);
	uint64_t native_before = native_seen[6];
	test_long_callsite(L);
	assert(calls(6) >= before+300u);
	assert(native_seen[6] >= native_before+200u);
  }
  test_other_callsite_footer_rejected(L);

  for (i = 1; i < 7; i++) {
    assert(native_seen[i] >= 200);
    assert(calls(i) >= 300);
  }
  assert(native_seen[7] == 0 && calls(7) == 0);
  install(NULL);
  lua_close(L);
  assert(dlclose(library) == 0);
  printf("t-arm64-jit-ffi-abi-classes OK: six-GPR, mixed GPR/FPR, three calls/loop and unused result; float return classifier checked\n");
  return 0;
}

#else
int main(void)
{
  puts("t-arm64-jit-ffi-abi-classes SKIP: requires experimental Darwin ARM64");
  return 0;
}
#endif
