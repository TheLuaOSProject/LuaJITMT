/* Independent semantic and post-RA admission for Lua INT CALLXS arguments. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) &&      \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) &&                                      \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) &&                               \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "INT CALLXS admission requires ARM64 CALLXS admission"
#endif

enum { KINDS = 8, FORMS = 3, SHAPES = KINDS * FORMS * 2 };
static const char *const kind_name[KINDS] = {"i8",  "u8",  "i16", "u16",
                                             "i32", "u32", "i64", "u64"};
static const uint8_t kind_width[KINDS] = {1, 1, 2, 2, 4, 4, 8, 8};
static const uint8_t kind_unsigned[KINDS] = {0, 1, 0, 1, 0, 1, 0, 1};

typedef struct IntFixture {
  lua_State *L;
  jit_State *J;
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nk, nins, ktrace;
  IRRef calls[4], xsaves[4], leaves[4];
  unsigned ncalls, form, kind, stack;
  IRRef selected, source, carrier, producer;
  unsigned producer_ix;
  IRRef args[16], nodes[16];
  unsigned nargs;
  BCReg arg_slot;
  MSize xsnap, arg_entry, nsnap, nsnapmap;
  GCtrace view_owner, semantic;
} IntFixture;

typedef struct JSave {
  GCtrace *curfinal;
  GCproto *pt;
  const BCIns *startpc;
  IRRef loopref;
  TraceNo parent;
  ExitNo exitno;
  BCReg baseslot;
  int32_t framedepth, retdepth;
  IRRef1 ktrace;
} JSave;

enum Mutation {
  M_SOURCE,
  M_CONV_MODE,
  M_CONV_TYPE,
  M_CARG_SOURCE,
  M_XSAVE_SOURCE,
  M_XSAVE_NORESTORE,
  M_XSAVE_TRUNCATED,
  M_BAD_BANK,
  M_BAD_SPILL_ALIGN,
  M_BAD_SPILL_EXTENT,
  M_PRODUCER_LINK,
  M_PRODUCER_DONE_WRONG,
  M_PRODUCER_DONE_NORESTORE,
  M_PRODUCER_DONE_TRUNCATED,
  M_ABI_SIGN,
  M_CALLM_SOURCE_PC,
  M_CALLM_ARG_COUNT,
  M_RAW_SOURCE,
  M_FIXED_SENTINEL,
  M_CARRIER_SNAPSHOT,
  M_LITERAL_TYPE,
  M_LITERAL_FLOAT,
  M_LITERAL_OPERAND,
  M_LITERAL_PAYLOAD
};

static unsigned semantic_ok, semantic_bad, postra_ok, postra_bad;

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  pt = funcproto(funcV(L->top - 1));
  lua_pop(L, 1);
  return pt;
}

static CTypeID global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  id = cdataV(L->top - 1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static unsigned op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r;
  unsigned n = 0;
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++)
    if (ir_load_acq(&ir[r]).o == op)
      n++;
  return n;
}

static IRRef find_ktrace(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r, nk = trace_nk_acq((GCtrace *)T);
  for (r = nk; r < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[r]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[r]) == obj2gco(T))
      return r;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      r += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      r++;
    else
      return 0;
  }
  return 0;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq((GCtrace *)T);
  IRRef n = trace_nins_acq((GCtrace *)T);
  while (n > REF_BASE) {
    IRIns x = ir_load_acq(&ir[n - 1u]);
    if (x.o == IR_RENAME && x.t.irt == IRT_NIL && x.op1 >= REF_FIRST &&
        x.op1 < n - 1u && x.op2 < trace_nsnap_acq((GCtrace *)T) &&
        x.s == SPS_NONE)
      n--;
    else if (x.o == IR_NOP && x.t.irt == IRT_NIL && !x.op1 && !x.op2 && !x.prev)
      n--;
    else
      break;
  }
  return n;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt, unsigned ncall)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) == ncall && op_count(T, IR_XSAVE) == ncall)
      return T;
  }
  return NULL;
}

static int snapshot_slot(IntFixture *fx, MSize sn, BCReg slot, MSize *entryp,
                         IRRef *refp)
{
  MSize ofs, n, i;
  unsigned found = 0;
  if (sn >= fx->nsnap)
    return 0;
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  n = snap_nent_acq(&fx->snap[sn]);
  if (ofs > fx->nsnapmap || n > fx->nsnapmap - ofs)
    return 0;
  for (i = 0; i < n; i++)
    if (snap_slot(fx->snapmap[ofs + i]) == slot) {
      if (found++)
        return 0;
      if (entryp)
        *entryp = ofs + i;
      if (refp)
        *refp = snap_ref(fx->snapmap[ofs + i]);
    }
  return found == 1;
}

static int snap_for_ref(IntFixture *fx, IRRef ref, MSize *out)
{
  MSize s;
  for (s = 0; s < fx->nsnap; s++)
    if (snap_ref_acq(&fx->snap[s]) == ref) {
      *out = s;
      return 1;
    }
  return 0;
}

static BCIns source_ins(IntFixture *fx, unsigned ix, const BCIns **retpc)
{
  IRIns mark = ir_load_acq(&fx->ir[fx->xsaves[ix]]);
  uint32_t off = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
  const BCIns *pc;
  assert(mark.o == IR_XSAVE && off > 0 && off < fx->pt->sizebc);
  pc = &proto_bc(fx->pt)[off - 1u];
  if (retpc)
    *retpc = &proto_bc(fx->pt)[off];
  return loadbc(pc);
}

static unsigned carg_values(IntFixture *fx, IRRef call, IRRef *values,
                            IRRef *nodes, unsigned cap)
{
  IRRef rev[32], rn[32], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  for (;;) {
    IRIns in;
    if (n >= cap || r >= call || r < fx->nk)
      return 0;
    in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) {
      rev[n] = r;
      rn[n++] = 0;
      break;
    }
    if (in.t.irt != IRT_NIL || in.op1 >= r || in.op2 >= r)
      return 0;
    rev[n] = in.op2;
    rn[n++] = r;
    r = in.op1;
  }
  for (i = 0; i < n; i++) {
    values[i] = rev[n - i - 1u];
    nodes[i] = rn[n - i - 1u];
  }
  return n;
}

static void locate_calls(IntFixture *fx)
{
  IRRef r;
  fx->ncalls = 0;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    IRRef p;
    if (in.o != IR_CALLXS)
      continue;
    assert(fx->ncalls < 4);
    fx->calls[fx->ncalls] = r;
    fx->xsaves[fx->ncalls] = fx->leaves[fx->ncalls] = 0;
    for (p = r; p > REF_FIRST;) {
      IRRef q = --p;
      if (ir_load_acq(&fx->ir[q]).o == IR_XSAVE) {
        fx->xsaves[fx->ncalls] = q;
        break;
      }
    }
    for (p = r + 1u; p < trace_nins_acq(fx->owner); p++) {
      IRIns x = ir_load_acq(&fx->ir[p]);
      if (x.o == IR_CALLS && x.op2 == IRCALL_lj_ffi_native_trace_leave) {
        fx->leaves[fx->ncalls] = p;
        break;
      }
    }
    assert(fx->xsaves[fx->ncalls] && fx->leaves[fx->ncalls]);
    fx->ncalls++;
  }
}

static void locate_target(IntFixture *fx)
{
  IRRef args[16], nodes[16], carrier, root;
  unsigned n;
  fx->selected = fx->calls[fx->ncalls - 1u];
  n = carg_values(fx, fx->selected, args, nodes, 16);
  fx->nargs = n;
  assert(n == (fx->stack ? 12u : 1u));
  memcpy(fx->args, args, n * sizeof(IRRef));
  memcpy(fx->nodes, nodes, n * sizeof(IRRef));
  carrier = args[n - 1u];
  fx->carrier = carrier;
  root = carrier;
  while (root >= REF_FIRST && root < trace_nins_acq(fx->owner)) {
    IRIns in = ir_load_acq(&fx->ir[root]);
    if (in.o != IR_CONV)
      break;
    assert(in.op1 < root);
    root = in.op1;
  }
  fx->source = root;
  assert(irt_type(ir_load_acq(&fx->ir[root]).t) == IRT_INT ||
         (fx->form == 3 && fx->ir[root].o == IR_KINT64 &&
          fx->ir[root].t.irt == IRT_I64) ||
         (fx->form == 2 && root == fx->producer));
  {
    IRRef i;
    const BCIns *retpc = NULL;
    BCIns src = source_ins(fx, fx->ncalls - 1u, &retpc);
    assert((fx->form == 2 ? bc_op(src) == BC_CALLM : bc_op(src) == BC_CALL));
    assert(fx->form == 2 ? bc_c(src) + 1u == n : bc_c(src) - 1u == n);
    if (bc_op(src) == BC_CALL)
      fx->arg_slot =
          (BCReg)(bc_a(src) + 2u * (1u + LJ_FR2) + 1u + (fx->stack ? 11u : 0u));
    else
      fx->arg_slot =
          (BCReg)((1u + LJ_FR2) + bc_a(src) + (1u + LJ_FR2) + bc_c(src) + 1u);
    assert(snap_for_ref(fx, fx->xsaves[fx->ncalls - 1u], &fx->xsnap));
    if (fx->form == 2) {
      fx->producer = 0;
      for (i = 0; i < fx->ncalls - 1u; i++) {
        const BCIns *pret = NULL;
        BCIns ps = source_ins(fx, i, &pret);
        if (bc_op(ps) == BC_CALL && retpc == pret + 1) {
          fx->producer = fx->calls[i];
          fx->producer_ix = i;
        }
      }
      assert(fx->producer && root == fx->producer);
    }
    {
      IRRef staged;
      assert(
          snapshot_slot(fx, fx->xsnap, fx->arg_slot, &fx->arg_entry, &staged));
      if (fx->form == 3) {
        assert(fx->ir[staged].o == IR_KINT &&
               fx->ir[staged].t.irt == IRT_INT &&
               fx->ir[root+1u].tv.u64 == (uint64_t)(int64_t)fx->ir[staged].i);
        fx->source = staged;
      } else assert(staged == root);
    }
    assert(!(fx->snapmap[fx->arg_entry] & SNAP_NORESTORE));
  }
}

static void assert_call_continuations(IntFixture *fx)
{
  for (unsigned i = 0; i < fx->ncalls; i++) {
    const BCIns *retpc = NULL;
    BCIns src = source_ins(fx, i, &retpc);
    MSize sn;
    UNUSED(src);
    assert(snap_for_ref(fx, fx->leaves[i], &sn));
    assert(snap_count_acq(&fx->snap[sn]) == SNAPCOUNT_DONE);
    {
      MSize ofs = snap_mapofs_acq(&fx->snap[sn]),
            n = snap_nent_acq(&fx->snap[sn]);
      assert(ofs <= fx->nsnapmap && n + 1u + LJ_FR2 <= fx->nsnapmap - ofs);
      assert(snap_pc(&fx->snapmap[ofs + n]) == retpc);
    }
    if (fx->form == 2 && bc_op(src) == BC_CALL) {
      assert(bc_b(src) == 0 && retpc < proto_bc(fx->pt) + fx->pt->sizebc);
      assert(bc_op(loadbc(retpc)) == BC_CALLM);
    }
  }
}

static void reset_fixture(IntFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner), full = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)full * sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
         (size_t)(full - nk) * sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)fx->nsnap * sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)fx->nsnapmap * sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  fx->nins = semantic_nins(fx->owner);
  locate_calls(fx);
  locate_target(fx);
}

static JSave enter_semantic(IntFixture *fx)
{
  jit_State *J = fx->J;
  JSave s = {J->curfinal, J->pt,       J->startpc,    J->loopref,  J->parent,
             J->exitno,   J->baseslot, J->framedepth, J->retdepth, J->ktrace};
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace + 1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = 0;
  for (IRRef r = REF_FIRST; r < fx->nins; r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) {
      J->loopref = r;
      break;
    }
  assert(J->loopref);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1u + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = fx->ktrace;
  return s;
}

static void leave_semantic(jit_State *J, JSave *s)
{
  J->curfinal = s->curfinal;
  J->pt = s->pt;
  J->startpc = s->startpc;
  J->loopref = s->loopref;
  J->parent = s->parent;
  J->exitno = s->exitno;
  J->baseslot = s->baseslot;
  J->framedepth = s->framedepth;
  J->retdepth = s->retdepth;
  J->ktrace = s->ktrace;
}

static LJArm64PostRAView postra_view(IntFixture *fx)
{
  LJArm64PostRAView v = {0};
  v.ir = fx->ir;
  v.owner = &fx->view_owner;
  v.cts = fx->cts;
  v.snap = fx->snap;
  v.snapmap = fx->snapmap;
  v.proto_bc = proto_bc(fx->pt);
  v.proto = fx->pt;
  v.startpc = trace_startpc_acq(fx->owner);
  v.nins = trace_nins_acq(&fx->view_owner);
  v.nk = fx->nk;
  v.nsnap = fx->nsnap;
  v.nsnapmap = fx->nsnapmap;
  v.spadjust = trace_spadjust_acq(fx->owner);
  v.proto_sizebc = fx->pt->sizebc;
  v.proto_numparams = fx->pt->numparams;
  v.root_topslot = trace_topslot_acq(fx->owner);
  v.startins = trace_startins_acq(fx->owner);
  return v;
}

static int semantic_accept(IntFixture *fx)
{
  uint32_t *pc = (uint32_t *)trace_startpc_acq(fx->owner),
           old = la_load32_acq(pc);
  JSave s;
  int got;
  la_store32_rel(pc, fx->owner->startins);
  s = enter_semantic(fx);
  got = lj_asm_arm64_ir_admit(fx->J, &fx->semantic, NULL);
  leave_semantic(fx->J, &s);
  la_store32_rel(pc, old);
  return got;
}

static int postra_accept(IntFixture *fx)
{
  LJArm64PostRAView v = postra_view(fx);
  return lj_asm_arm64_postra_admit(&v, NULL);
}

static void assert_abi(lua_State *L, unsigned kind, unsigned stack)
{
  char name[48];
  LJArm64ABISignature abi;
  CTypeID id;
  unsigned arg = stack ? 11u : 0u;
  (void)snprintf(name, sizeof(name), "__int_arg_%s%s", stack ? "stack_" : "",
                 kind_name[kind]);
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  id = cdataV(L->top - 1)->ctypeid;
  lua_pop(L, 1);
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)), id, &abi));
  assert(!abi.is_vararg && abi.result_class == LJ_ARM64_ABI_FPR &&
         abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT && abi.result_size == 8);
  assert(abi.nargs == (stack ? 12u : 1u));
  assert(abi.arg[arg].kind == LJ_ARM64_ABI_KIND_INTEGER &&
         abi.arg[arg].size == kind_width[kind] &&
         abi.arg[arg].is_unsigned == kind_unsigned[kind]);
}

static IRRef find_other_int(IntFixture *fx)
{
  for (IRRef r = REF_FIRST; r < fx->selected; r++) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (r != fx->source && in.t.irt == IRT_INT &&
        (in.o == IR_SLOAD || in.o == IR_KINT || in.o == IR_VLOAD))
      return r;
  }
  return 0;
}

static void replace_selected_arg(IntFixture *fx, IRRef value)
{
  IRRef node = fx->nodes[fx->nargs - 1u];
  if (node)
    fx->ir[node].op2 = value;
  else
    fx->ir[fx->selected].op1 = value;
}

static void carrier_in_unrelated_snapshot(IntFixture *fx)
{
  BCIns src = source_ins(fx, fx->ncalls - 1u, NULL);
  for (MSize s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    MSize ofs = snap_mapofs_acq(&fx->snap[s]), n = snap_nent_acq(&fx->snap[s]);
    if (at <= fx->carrier || ofs > fx->nsnapmap || n > fx->nsnapmap - ofs)
      continue;
    for (MSize i = 0; i < n; i++) {
      SnapEntry e = fx->snapmap[ofs + i];
      BCReg slot = snap_slot(e);
      IRRef old = snap_ref(e);
      if (slot >= bc_a(src) || e != SNAP(slot, 0, old) || old < fx->nk ||
          old >= at || old == fx->source ||
          irt_type(ir_load_acq(&fx->ir[old]).t) != IRT_INT)
        continue;
      fx->snapmap[ofs + i] = snap_setref(e, fx->carrier);
      return;
    }
  }
  assert(!"no unrelated loop INT slot to test native carrier snapshot leak");
}

static void mutate(IntFixture *fx, enum Mutation m)
{
  IRRef r = fx->carrier, chain[8];
  unsigned n = 0;
  IRRef other;
  while (r >= REF_FIRST && r < trace_nins_acq(fx->owner) &&
         ir_load_acq(&fx->ir[r]).o == IR_CONV) {
    assert(n < 8);
    chain[n++] = r;
    r = ir_load_acq(&fx->ir[r]).op1;
  }
  switch (m) {
  case M_LITERAL_TYPE:
    fx->ir[fx->carrier].t.irt = IRT_P64;
    break;
  case M_LITERAL_FLOAT:
    fx->ir[fx->carrier].o = IR_KNUM;
    fx->ir[fx->carrier].t.irt = IRT_NUM;
    break;
  case M_LITERAL_OPERAND:
    replace_selected_arg(fx, REF_TRUE);
    break;
  case M_LITERAL_PAYLOAD:
    fx->ir[fx->carrier+1u].tv.u64 ^= UINT64_C(1);
    break;
  case M_SOURCE:
    other = find_other_int(fx);
    assert(other);
    if (n)
      fx->ir[chain[n - 1u]].op1 = other;
    else
      replace_selected_arg(fx, other);
    break;
  case M_CONV_MODE:
    assert(n);
    fx->ir[chain[0]].op2 ^= IRCONV_SEXT;
    break;
  case M_CONV_TYPE:
    assert(n);
    fx->ir[chain[0]].t.irt = fx->kind & 1u ? IRT_I64 : IRT_U64;
    break;
  case M_CARG_SOURCE:
    assert(fx->stack && fx->nargs == 12);
    other = fx->args[0];
    replace_selected_arg(fx, other);
    break;
  case M_RAW_SOURCE:
    assert(fx->carrier != fx->source);
    /* A hoisted conversion may feed both unrolled calls. Remove every use
    ** and its definition, so rejection cannot rely on an unused carrier. */
    for (IRRef q = fx->carrier + 1u; q < fx->nins; q++) {
      IRIns *in = &fx->ir[q];
      if (in->o == IR_CARG) {
        if (in->op1 == fx->carrier)
          in->op1 = fx->source;
        if (in->op2 == fx->carrier)
          in->op2 = fx->source;
      } else if (in->o == IR_CALLXS && in->op1 == fx->carrier)
        in->op1 = fx->source;
    }
    memset(&fx->ir[fx->carrier], 0, sizeof(IRIns));
    fx->ir[fx->carrier].o = IR_NOP;
    fx->ir[fx->carrier].t.irt = IRT_NIL;
    break;
  case M_FIXED_SENTINEL:
    assert(fx->stack && fx->nodes[1] && !fx->nodes[0] &&
           fx->args[0] != fx->args[1]);
    fx->ir[fx->nodes[1]].op1 = fx->args[1];
    break;
  case M_CARRIER_SNAPSHOT:
    assert(irt_type(ir_load_acq(&fx->ir[fx->carrier]).t) == IRT_INT);
    carrier_in_unrelated_snapshot(fx);
    break;
  case M_XSAVE_SOURCE:
  case M_XSAVE_NORESTORE:
  case M_XSAVE_TRUNCATED: {
    if (m == M_XSAVE_TRUNCATED)
      fx->snap[fx->xsnap].nslots = fx->arg_slot;
    else if (m == M_XSAVE_NORESTORE)
      fx->snapmap[fx->arg_entry] |= SNAP_NORESTORE;
    else {
      other = find_other_int(fx);
      assert(other);
      fx->snapmap[fx->arg_entry] =
          snap_setref(fx->snapmap[fx->arg_entry], other);
    }
    break;
  }
  case M_PRODUCER_LINK:
    assert(fx->form == 2 && fx->producer);
    other = 0;
    for (unsigned i = 0; i < fx->ncalls; i++)
      if (i != fx->producer_ix && bc_op(source_ins(fx, i, NULL)) == BC_CALL) {
        other = fx->calls[i];
        break;
      }
    assert(other);
    replace_selected_arg(fx, other);
    break;
  case M_ABI_SIGN: {
    char name[48];
    CTypeID id;
    IRRef fn = ir_load_acq(&fx->ir[fx->selected]).op2;
    IRIns f = ir_load_acq(&fx->ir[fn]);
    IRRef cdata = f.o == IR_FLOAD ? f.op1 : 0, k = 0;
    unsigned opposite = fx->kind ^ 1u;
    (void)snprintf(name, sizeof(name), "__int_arg_%s%s",
                   fx->stack ? "stack_" : "", kind_name[opposite]);
    id = global_ctype(fx->L, name);
    assert(cdata);
    for (IRRef r = REF_FIRST; r < fx->selected && !k; r++) {
      IRIns fld = ir_load_acq(&fx->ir[r]);
      if (fld.o != IR_FLOAD || fld.op1 != cdata ||
          fld.op2 != IRFL_CDATA_CTYPEID)
        continue;
      for (IRRef q = r + 1u; q < fx->selected; q++) {
        IRIns eq = ir_load_acq(&fx->ir[q]);
        IRRef c;
        if (eq.o != IR_EQ || !irt_isguard(eq.t) || (eq.op1 != r && eq.op2 != r))
          continue;
        c = eq.op1 == r ? eq.op2 : eq.op1;
        if (c >= fx->nk && ir_load_acq(&fx->ir[c]).o == IR_KINT) {
          k = c;
          break;
        }
      }
    }
    assert(k);
    fx->ir[k].i = (int32_t)id;
    break;
  }
  case M_PRODUCER_DONE_WRONG:
  case M_PRODUCER_DONE_NORESTORE:
  case M_PRODUCER_DONE_TRUNCATED: {
    BCReg slot;
    MSize sn, ofs, nent, i;
    IRRef repl;
    const BCIns *retpc = NULL;
    BCIns src = source_ins(fx, fx->producer_ix, &retpc);
    assert(fx->form == 2 && bc_op(src) == BC_CALL && bc_b(src) == 0);
    slot = (BCReg)(1u + LJ_FR2 + bc_a(src));
    assert(snap_for_ref(fx, fx->leaves[fx->producer_ix], &sn));
    ofs = snap_mapofs_acq(&fx->snap[sn]);
    nent = snap_nent_acq(&fx->snap[sn]);
    if (m == M_PRODUCER_DONE_TRUNCATED) {
      fx->snap[sn].nslots = slot;
      break;
    }
    for (i = 0; i < nent; i++)
      if (snap_slot(fx->snapmap[ofs + i]) == slot) {
        if (m == M_PRODUCER_DONE_NORESTORE)
          fx->snapmap[ofs + i] |= SNAP_NORESTORE;
        else {
          repl = find_other_int(fx);
          assert(repl);
          fx->snapmap[ofs + i] = snap_setref(fx->snapmap[ofs + i], repl);
        }
        break;
      }
    assert(i < nent);
    break;
  }
  case M_CALLM_SOURCE_PC: {
    const BCIns *pc = NULL;
    (void)source_ins(fx, fx->ncalls - 1u, &pc);
    assert(fx->form == 2 && pc);
    pc--;
    assert(bc_op(loadbc(pc)) == BC_CALLM);
    {
      BCIns old = loadbc(pc);
      BCIns *mut = (BCIns *)(uintptr_t)pc;
      bc_publish((const uint32_t *)mut, (old & UINT32_C(0xffffff00)) | BC_CALL);
    }
    break;
  }
  case M_CALLM_ARG_COUNT: {
    const BCIns *pc = NULL;
    (void)source_ins(fx, fx->ncalls - 1u, &pc);
    assert(fx->form == 2 && pc);
    pc--;
    assert(bc_op(loadbc(pc)) == BC_CALLM);
    {
      BCIns old = loadbc(pc);
      unsigned c = bc_c(old);
      assert(c < UINT8_MAX);
      bc_publish((const uint32_t *)(uintptr_t)pc,
                 (old & UINT32_C(0xff00ffff)) | ((c + 1u) << 16));
    }
    break;
  }
  case M_BAD_BANK:
    fx->ir[fx->carrier].r = RID_D0;
    fx->ir[fx->carrier].s = SPS_NONE;
    break;
  case M_BAD_SPILL_ALIGN:
    fx->ir[fx->carrier].r = RID_NONE;
    fx->ir[fx->carrier].s = (uint8_t)(SPS_FIRST + 1u);
    break;
  case M_BAD_SPILL_EXTENT: {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner) / sizeof(int32_t);
    assert(cap < UINT8_MAX);
    fx->ir[fx->carrier].r = RID_NONE;
    fx->ir[fx->carrier].s = (uint8_t)cap;
    break;
  }
  }
}

static void expect_both(IntFixture *fx, enum Mutation m)
{
  const BCIns *mutpc = NULL;
  BCIns original = 0;
  int changes_bytecode;
  changes_bytecode = (m == M_CALLM_SOURCE_PC || m == M_CALLM_ARG_COUNT);
  if (changes_bytecode) {
    (void)source_ins(fx, fx->ncalls - 1u, &mutpc);
    mutpc--;
    original = loadbc(mutpc);
  }
  reset_fixture(fx);
  mutate(fx, m);
  if (semantic_accept(fx)) {
    fprintf(stderr, "INT semantic accepted mutation %d form%u kind%u stack%u\n",
            m, fx->form, fx->kind, fx->stack);
    assert(0);
  }
  semantic_bad++;
  if (changes_bytecode)
    bc_publish((const uint32_t *)mutpc, original);
  reset_fixture(fx);
  mutate(fx, m);
  if (postra_accept(fx)) {
    fprintf(stderr, "INT post-RA accepted mutation %d form%u kind%u stack%u\n",
            m, fx->form, fx->kind, fx->stack);
    assert(0);
  }
  postra_bad++;
  if (changes_bytecode)
    bc_publish((const uint32_t *)mutpc, original);
}

static void capture(IntFixture *fx, lua_State *L, jit_State *J,
                    const char *runner, unsigned form, unsigned kind,
                    unsigned stack, unsigned ncalls)
{
  fx->L = L;
  fx->J = J;
  fx->form = form;
  fx->kind = kind;
  fx->stack = stack;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt, ncalls);
  if (!fx->owner)
    fprintf(stderr, "INT admission trace missing: %s\n", runner);
  assert(fx->owner);
  fx->cts = ctype_ctsG(G(L));
  fx->nk = trace_nk_acq(fx->owner);
  fx->ktrace = find_ktrace(fx->owner);
  fx->nsnap = trace_nsnap_acq(fx->owner);
  fx->nsnapmap = trace_nsnapmap_acq(fx->owner);
  assert(fx->cts && fx->ktrace && fx->nsnap && fx->nsnapmap);
  fx->ir = (IRIns *)calloc(trace_nins_acq(fx->owner), sizeof(IRIns));
  fx->snap = (SnapShot *)calloc(fx->nsnap, sizeof(SnapShot));
  fx->snapmap = (SnapEntry *)calloc(fx->nsnapmap, sizeof(SnapEntry));
  assert(fx->ir && fx->snap && fx->snapmap);
  reset_fixture(fx);
}

static void free_fixture(IntFixture *fx)
{
  free(fx->snapmap);
  free(fx->snap);
  free(fx->ir);
}

static void run_runner(lua_State *L, const char *name)
{
  char consumer[80];
  unsigned kind = 0, form = 0, stack = 0;
  assert(sscanf(name, "__int_arg_run_k%u_f%u_s%u", &kind, &form, &stack) == 3);
  assert(kind < KINDS && form <= FORMS && stack < 2);
  snprintf(consumer, sizeof(consumer), "__int_arg_%s%s", stack ? "stack_" : "",
           kind_name[kind]);
  lua_getglobal(L, "__int_arg_configure");
  lua_pushinteger(L, kind);
  lua_pushboolean(L, stack);
  assert(lua_pcall(L, 2, 0, 0) == LUA_OK);
  lua_getglobal(L, "__int_arg_driver");
  lua_getglobal(L, name);
  lua_getglobal(L, "__int_arg_table");
  lua_pushinteger(L, -129);
  lua_getglobal(L, consumer);
  lua_getglobal(L, "__int_arg_producer");
  lua_pushinteger(L, 24);
  if (lua_pcall(L, 6, 1, 0) != LUA_OK) {
    fprintf(stderr, "INT runner %s: %s\n", name, lua_tostring(L, -1));
    abort();
  }
  assert(lua_isnumber(L, -1) && lua_tonumber(L, -1) == 30.0);
  lua_pop(L, 1);
}

static void test_case(lua_State *L, jit_State *J, const char *runner,
                      unsigned form, unsigned kind, unsigned stack)
{
  IntFixture fx = {0};
  GCproto *pt = global_proto(L, runner);
  unsigned ncalls = form == 2 ? 4 : 2, attempt;
  assert_abi(L, kind, stack);
  for (attempt = 0; attempt < 8 && !find_trace(J, pt, ncalls); attempt++) {
    ljt_lua_dostring(
        L, "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')");
    run_runner(L, runner);
  }
  capture(&fx, L, J, runner, form, kind, stack, ncalls);
  assert(fx.ncalls == ncalls && fx.nargs == (stack ? 12u : 1u));
  assert_call_continuations(&fx);
  assert(semantic_accept(&fx));
  semantic_ok++;
  reset_fixture(&fx);
  assert(postra_accept(&fx));
  postra_ok++;
  /* Exercise provenance separately for each source family, then the encoded
  ** integer conversion modes across the signed/unsigned width matrix. */
  /* The pre-existing direct signed-i32 SLOAD path does not require a rooted
  ** argument proof. All newly admitted paths do, as do table and CALLM calls.
  * */
  if (!(form == 1 && kind == 4 && !stack))
    expect_both(&fx, M_SOURCE);
  if (kind != 4 && kind != 5)
    expect_both(&fx, M_CONV_MODE);
  if (kind != 4 && kind != 5)
    expect_both(&fx, M_CONV_TYPE);
  if (stack)
    expect_both(&fx, M_CARG_SOURCE);
  if (kind < 4 && fx.carrier != fx.source)
    expect_both(&fx, M_RAW_SOURCE);
  if (stack)
    expect_both(&fx, M_FIXED_SENTINEL);
  if (kind < 4 && !stack)
    expect_both(&fx, M_CARRIER_SNAPSHOT);
  if (!(form == 1 && kind == 4 && !stack)) {
    expect_both(&fx, M_XSAVE_NORESTORE);
    expect_both(&fx, M_XSAVE_SOURCE);
    expect_both(&fx, M_XSAVE_TRUNCATED);
  }
  if ((kind < 4 && !stack) || kind >= 6)
    expect_both(&fx, M_ABI_SIGN);
  else {
    /* Small stack stores and raw i32/u32 have the same carrier bits. A
    ** matching CType guard for the other signedness remains a valid graph. */
    reset_fixture(&fx);
    mutate(&fx, M_ABI_SIGN);
    assert(semantic_accept(&fx));
    semantic_ok++;
    reset_fixture(&fx);
    mutate(&fx, M_ABI_SIGN);
    assert(postra_accept(&fx));
    postra_ok++;
  }
  if (form == 2) {
    expect_both(&fx, M_PRODUCER_LINK);
    expect_both(&fx, M_PRODUCER_DONE_WRONG);
    expect_both(&fx, M_PRODUCER_DONE_NORESTORE);
    expect_both(&fx, M_PRODUCER_DONE_TRUNCATED);
    expect_both(&fx, M_CALLM_SOURCE_PC);
    expect_both(&fx, M_CALLM_ARG_COUNT);
  }
  reset_fixture(&fx);
  fx.ir[fx.carrier].r = RID_D0;
  fx.ir[fx.carrier].s = SPS_NONE;
  if (!postra_accept(&fx)) {
    postra_bad++;
  } else
    assert(!"wrong bank accepted");
  if (kind >= 6) {
    reset_fixture(&fx);
    fx.ir[fx.carrier].r = RID_NONE;
    fx.ir[fx.carrier].s = (uint8_t)(SPS_FIRST + 1u);
    assert(!postra_accept(&fx));
    postra_bad++;
  }
  {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx.owner) / sizeof(int32_t);
    assert(cap < UINT8_MAX);
    reset_fixture(&fx);
    fx.ir[fx.carrier].r = RID_NONE;
    fx.ir[fx.carrier].s = (uint8_t)cap;
    assert(!postra_accept(&fx));
    postra_bad++;
  }
  free_fixture(&fx);
}

static void test_literal_case(lua_State *L, jit_State *J, const char *runner,
                              unsigned kind, unsigned stack)
{
  IntFixture fx = {0};
  GCproto *pt = global_proto(L, runner);
  for (unsigned attempt = 0; attempt < 8 && !find_trace(J, pt, 2); attempt++) {
    ljt_lua_dostring(L, "jit.opt.start('hotloop=1','hotexit=1000')");
    run_runner(L, runner);
  }
  capture(&fx, L, J, runner, 3, kind, stack, 2);
  assert(semantic_accept(&fx)); semantic_ok++;
  reset_fixture(&fx);
  assert(postra_accept(&fx)); postra_ok++;
  expect_both(&fx, M_LITERAL_TYPE);
  expect_both(&fx, M_LITERAL_FLOAT);
  expect_both(&fx, M_LITERAL_OPERAND);
  /* The compact-stack grammar binds all argument carriers to their source
  ** snapshot; a different full-width constant must not satisfy that proof. */
  if (stack) expect_both(&fx, M_LITERAL_PAYLOAD);
  free_fixture(&fx);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  const char *so = getenv("LJ_M7_FFI_INT_ARGS_SO");
  assert(so && *so);
  ljt_lua_dostring(
      L,
      "local ffi=require('ffi'); local threading=require('threading')\n"
      "ffi.cdef[[void lj_ffi_int_configure(int32_t,int32_t); double "
      "lj_ffi_int_i8(int8_t); double lj_ffi_int_u8(uint8_t); double "
      "lj_ffi_int_i16(int16_t); double lj_ffi_int_u16(uint16_t); double "
      "lj_ffi_int_i32(int32_t); double lj_ffi_int_u32(uint32_t); double "
      "lj_ffi_int_i64(int64_t); double lj_ffi_int_u64(uint64_t); double "
      "lj_ffi_int_stack_i8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int8_t); double "
      "lj_ffi_int_stack_u8(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint8_t); double "
      "lj_ffi_int_stack_i16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int16_t); double "
      "lj_ffi_int_stack_u16(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint16_t); double "
      "lj_ffi_int_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t); double "
      "lj_ffi_int_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t); double "
      "lj_ffi_int_stack_i64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,int64_t); double "
      "lj_ffi_int_stack_u64(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,"
      "int32_t,int32_t,uint8_t,int16_t,uint32_t,uint64_t); int32_t "
      "lj_ffi_int_producer(int32_t);]]\n"
      "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_INT_ARGS_SO'))); local "
      "names={'i8','u8','i16','u16','i32','u32','i64','u64'}\n"
      "for _,k in ipairs(names) do _G['__int_arg_'..k]=lib['lj_ffi_int_'..k]; "
      "_G['__int_arg_stack_'..k]=lib['lj_ffi_int_stack_'..k] end; "
      "_G.__int_arg_producer=lib.lj_ffi_int_producer; "
      "_G.__int_arg_table={value=-129}\n"
      "function __int_arg_configure(k,s) lib.lj_ffi_int_configure(k,s and 1 or "
      "0) end\n"
      "function __int_arg_install(id,form,stack,k,literal) local x=(form==0 and "
      "'t.value' or 'v'); if form==2 then x='p('..x..')' end; "
      "if form==3 then x=tostring(literal) end; local a=stack "
      "and ('101,102,103,104,105,106,107,108,31,-505,1985229328,'..x) or x; "
      "local b='local sum=.5; for i=1,n do sum=sum+c('..a..') end; return "
      "sum-.5'; local p='t,v,c,p,n'; "
      "_G['__int_arg_run_'..id]=assert(loadstring('return function('..p..') "
      "'..b..' end','@intarg-'..id))(); end\n"
      "function __int_arg_driver(run,t,v,c,p,n) local r=run(t,v,c,p,n); return "
      "r end; __int_arg_ready=threading.channel(1); "
      "__int_arg_stop=threading.channel(1); "
      "__int_arg_peer=assert(threading.spawn(function(held,a,b) local "
      "x=held.value; assert(a:send(true,5)); assert(select(2,b:recv(60))); "
      "assert(held.value==x); return true "
      "end,__int_arg_table,__int_arg_ready,__int_arg_stop)); "
      "assert(select(2,__int_arg_ready:recv(5))); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')\n");
  for (unsigned form = 0; form < FORMS; form++)
    for (unsigned kind = 0; kind < KINDS; kind++)
      for (unsigned stack = 0; stack < 2; stack++) {
        char id[48], runner[80];
        snprintf(id, sizeof(id), "k%u_f%u_s%u", kind, form, stack);
        lua_getglobal(L, "__int_arg_install");
        lua_pushstring(L, id);
        lua_pushinteger(L, form);
        lua_pushboolean(L, stack);
        lua_pushinteger(L, kind);
        assert(lua_pcall(L, 4, 0, 0) == LUA_OK);
        snprintf(runner, sizeof(runner), "__int_arg_run_%s", id);
        test_case(L, J, runner, form, kind, stack);
      }
  for (unsigned kind = 6; kind < KINDS; kind++)
    for (unsigned stack = 0; stack < 2; stack++) {
      static const lua_Integer value[] = {-129, INT32_MIN, INT32_MAX, 1};
      for (unsigned vi = 0; vi < sizeof(value)/sizeof(value[0]); vi++) {
        char id[64], runner[96];
        snprintf(id, sizeof(id), "k%u_f3_s%u_v%u", kind, stack, vi);
        lua_getglobal(L, "__int_arg_install");
        lua_pushstring(L, id);
        lua_pushinteger(L, 3);
        lua_pushboolean(L, stack);
        lua_pushinteger(L, kind);
        lua_pushinteger(L, value[vi]);
        assert(lua_pcall(L, 5, 0, 0) == LUA_OK);
        snprintf(runner, sizeof(runner), "__int_arg_run_%s", id);
        test_literal_case(L, J, runner, kind, stack);
      }
    }
  ljt_lua_dostring(L, "assert(__int_arg_stop:send(true,5)); local "
                      "ok,v=__int_arg_peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("INT CALLXS semantic=%u/%u post-RA=%u/%u\n", semantic_ok, semantic_bad,
         postra_ok, postra_bad);
  puts("t-arm64-jit-ffi-int-args-admission OK");
  return 0;
}
#else
int main(void)
{
  puts("t-arm64-jit-ffi-int-args-admission SKIP");
  return 0;
}
#endif
