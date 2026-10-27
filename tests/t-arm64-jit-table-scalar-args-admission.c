/* Independent admission proofs for scalar values read from shared tables and
** passed as arguments to fixed-signature ARM64 CALLXS traces. */

#include <assert.h>
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
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_XSAVE_TEST_HELPERS)

#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_ccall_arm64.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "table scalar CALLXS admission requires ARM64 CALLXS admission"
#endif

typedef struct ScalarRead {
  IRRef call, value, root1, root2, status, carrier, lookup, xsave;
  IRRef ctype, ctype_guard, ctype_const;
  MSize pre, read, after, xsnap;
  MSize result_entry, arg_entry, table_entry, key_entry;
  BCReg result_slot, arg_slot, table_slot, key_slot;
  unsigned arg;
  IRType result_type;
  const BCIns *read_pc, *call_pc;
} ScalarRead;

typedef struct ScalarFixture {
  lua_State *L;
  jit_State *J;
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, ktrace;
  MSize nsnap, nsnapmap;
  GCtrace view_owner, semantic;
  ScalarRead read;
  const char *runner, *fn;
  unsigned arg;
} ScalarFixture;

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
  M_HELPER, M_STATUS, M_ROOT1_MODE, M_ROOT2_MODE, M_CARG_ROOT,
  M_TABLE_SOURCE, M_VLOAD_BASE, M_VLOAD_OFFSET, M_VLOAD_TYPE,
  M_VLOAD_GUARD, M_READ_PC, M_READ_MAP, M_RESULT_MAP, M_RESULT_PC,
  M_CALLARG_SOURCE, M_XSAVE_SOURCE, M_XSAVE_NORESTORE, M_XSAVE_TRUNCATED,
  M_CONV_SOURCE, M_CONV_MODE, M_ABI_TYPE, M_BOOL_TAG, M_BOOL_VALUE,
  M_ROOT_ALIAS, M_ROOT_SPILL, M_VALUE_SPILL_HALF, M_VALUE_SPILL_OUTSIDE,
  M_VALUE_WRONG_BANK, M_CALL_RESULT_BANK
};

static unsigned positive_cases, semantic_rejections, allocated_rejections;

static BCIns loadbc(const BCIns *pc)
{ return (BCIns)la_load32_acq((const uint32_t *)pc); }

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCproto *pt;
  lua_getglobal(L, name);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  lua_pop(L, 1);
  return pt;
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

static unsigned op_count(const GCtrace *T, IROp op)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r;
  unsigned n = 0;
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++)
    if (ir_load_acq(&ir[r]).o == op) n++;
  return n;
}

static unsigned read_count(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r;
  unsigned n = 0;
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++) {
    IRIns in = ir_load_acq(&ir[r]);
    if (in.o == IR_CALLS &&
        (in.op2 == IRCALL_lj_tab_getscalar_rooted_try ||
         in.op2 == IRCALL_lj_tab_gettv_rooted_hit_try)) n++;
  }
  return n;
}

static IRRef find_ktrace(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r, nk = trace_nk_acq((GCtrace *)T);
  for (r = nk; r < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[r]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[r]) == obj2gco(T)) return r;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR))) r += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL) r++;
    else return 0;
  }
  return 0;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq((GCtrace *)T);
  IRRef n = trace_nins_acq((GCtrace *)T);
  while (n > REF_BASE) {
    IRIns tail = ir_load_acq(&ir[n-1u]);
    if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL &&
        tail.op1 >= REF_FIRST && tail.op1 < n-1u &&
        tail.op2 < trace_nsnap_acq((GCtrace *)T) && tail.s == SPS_NONE) n--;
    else if (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
             tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0) n--;
    else break;
  }
  return n;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    if (T && trace_runnable_acq(T, no) && trace_root_acq(T) == 0 &&
        trace_startpt_acq(T) == pt &&
        bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) >= 1u && read_count(T) >= 1u)
      return T;
  }
  return NULL;
}

static int snapshot_slot(ScalarFixture *fx, MSize sn, BCReg slot,
                         MSize *entryp, IRRef *refp)
{
  MSize ofs, nent, i;
  unsigned found = 0;
  if (sn >= fx->nsnap) return 0;
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  nent = snap_nent_acq(&fx->snap[sn]);
  if (ofs > fx->nsnapmap || nent > fx->nsnapmap-ofs) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry se = fx->snapmap[ofs+i];
    if (snap_slot(se) == slot) {
      if (found++) return 0;
      if (entryp) *entryp = ofs+i;
      if (refp) *refp = snap_ref(se);
    }
  }
  return found == 1;
}

static int snap_for_ref(ScalarFixture *fx, IRRef ref, MSize *out)
{
  MSize s;
  for (s = 0; s < fx->nsnap; s++)
    if (snap_ref_acq(&fx->snap[s]) == ref) { *out = s; return 1; }
  return 0;
}

static int snap_before(ScalarFixture *fx, IRRef ref, MSize *out)
{
  MSize s, best = UINT_MAX;
  IRRef bestref = 0;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at <= ref && at >= bestref) { bestref = at; best = s; }
  }
  if (best == UINT_MAX) return 0;
  *out = best;
  return 1;
}

static int snap_after(ScalarFixture *fx, IRRef ref, MSize *out)
{
  MSize s, best = UINT_MAX;
  IRRef bestref = REF_DROP;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at > ref && at < bestref) { bestref = at; best = s; }
  }
  if (best == UINT_MAX) return 0;
  *out = best;
  return 1;
}

static const BCIns *footer_pc(ScalarFixture *fx, MSize sn)
{
  MSize ofs, nent;
  if (sn >= fx->nsnap) return NULL;
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  nent = snap_nent_acq(&fx->snap[sn]);
  if (ofs > fx->nsnapmap || nent > fx->nsnapmap-ofs ||
      1u+LJ_FR2 > fx->nsnapmap-ofs-nent) return NULL;
  return snap_pc(&fx->snapmap[ofs+nent]);
}

static void set_footer_pc(ScalarFixture *fx, MSize sn, const BCIns *pc)
{
  MSize ofs, nent;
  assert(sn < fx->nsnap && pc != NULL);
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  nent = snap_nent_acq(&fx->snap[sn]);
  assert(ofs <= fx->nsnapmap && nent <= fx->nsnapmap-ofs &&
         1u+LJ_FR2 <= fx->nsnapmap-ofs-nent);
#if LJ_FR2
  {
    SnapEntry *p = &fx->snapmap[ofs+nent];
    uint64_t packed;
    memcpy(&packed, p, sizeof(packed));
    packed = ((uint64_t)(uintptr_t)pc << 8) | (packed & 0xffu);
    memcpy(p, &packed, sizeof(packed));
  }
#else
  fx->snapmap[ofs+nent] = SNAP_MKPC(pc);
#endif
}

static int snapshots_equal(ScalarFixture *fx, MSize a, MSize b)
{
  MSize oa, ob, na, nb, i;
  if (a >= fx->nsnap || b >= fx->nsnap ||
      fx->snap[a].nslots != fx->snap[b].nslots ||
      fx->snap[a].topslot != fx->snap[b].topslot) return 0;
  oa = snap_mapofs_acq(&fx->snap[a]); ob = snap_mapofs_acq(&fx->snap[b]);
  na = snap_nent_acq(&fx->snap[a]); nb = snap_nent_acq(&fx->snap[b]);
  if (na != nb || oa > fx->nsnapmap || na > fx->nsnapmap-oa ||
      ob > fx->nsnapmap || nb > fx->nsnapmap-ob) return 0;
  for (i = 0; i < na; i++)
    if (fx->snapmap[oa+i] != fx->snapmap[ob+i]) return 0;
  return 1;
}

static int safe_setup_op(BCOp op)
{
  return op == BC_MOV || op == BC_KPRI || op == BC_KSHORT ||
         op == BC_KNUM || op == BC_KSTR || op == BC_KCDATA ||
         op == BC_KNIL || op == BC_CGET;
}

static int safe_setup_range(const BCIns *from, const BCIns *to)
{
  const BCIns *pc;
  if (from >= to) return 0;
  for (pc = from+1u; pc < to; pc++)
    if (!safe_setup_op(bc_op(loadbc(pc)))) return 0;
  return 1;
}

static unsigned carg_values(ScalarFixture *fx, IRRef call, IRRef *values,
                            unsigned cap)
{
  IRRef rev[32], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  while (r >= REF_FIRST && r < call) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) break;
    if (in.t.irt != IRT_NIL || n+1u >= cap || in.op1 >= r || in.op2 >= r)
      return 0;
    rev[n++] = in.op2;
    r = in.op1;
  }
  if (r < trace_nk_acq(fx->owner) || r >= call || n >= cap) return 0;
  values[0] = r;
  for (i = 0; i < n; i++) values[i+1u] = rev[n-i-1u];
  return n+1u;
}

static int carrier_from_value(ScalarFixture *fx, IRRef carrier,
                              IRRef value, IRType type)
{
  IRIns c;
  if (carrier == value) return 1;
  if (carrier < trace_nk_acq(fx->owner) ||
      carrier >= trace_nins_acq(fx->owner)) return 0;
  c = ir_load_acq(&fx->ir[carrier]);
  if (type == IRT_TRUE || type == IRT_FALSE) {
    if (carrier == (type == IRT_TRUE ? REF_TRUE : REF_FALSE)) return 1;
    if (c.o == IR_KINT && c.t.irt == IRT_INT &&
        c.i == (type == IRT_TRUE ? 1 : 0)) return 1;
  }
  return c.o == IR_CONV && c.op1 == value && c.op1 < carrier;
}

static IRRef carg_node(ScalarFixture *fx, IRRef call, unsigned arg)
{
  IRRef nodes[32], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0;
  while (r >= REF_FIRST && r < call) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) break;
    if (n >= 32) return 0;
    nodes[n++] = r;
    r = in.op1;
  }
  if (arg > n) return 0;
  if (n == 0) return arg == 0 ? call : 0;
  return arg == 0 ? nodes[n-1u] : nodes[n-arg];
}

static int scalar_abi_arg_matches(ScalarFixture *fx, IRRef call,
                                  unsigned arg, IRRef carrier)
{
  LJArm64ABISignature abi;
  IRType type;
  LJArm64ABIKind kind;
  unsigned size;
  lua_State *L = fx->L;
  CTypeID id = global_ctype(L, fx->fn);
  UNUSED(call);
  if (!lj_ccall_arm64_signature(fx->cts, id, &abi) || arg >= abi.nargs)
    return 0;
  if (carrier == REF_TRUE || carrier == REF_FALSE) type = IRT_INT;
  else if (carrier < trace_nk_acq(fx->owner)) {
    if (carrier >= REF_TRUE) return 0;
    type = irt_type(ir_load_acq(&fx->ir[carrier]).t);
    if (irref_isk(carrier) && ir_load_acq(&fx->ir[carrier]).o == IR_KINT)
      type = IRT_INT;
  } else {
    type = irt_type(ir_load_acq(&fx->ir[carrier]).t);
  }
  if (type == IRT_INT) kind = LJ_ARM64_ABI_KIND_INTEGER, size = 4;
  else if (type == IRT_NUM) kind = LJ_ARM64_ABI_KIND_FLOAT, size = 8;
  else if (type == IRT_FLOAT) kind = LJ_ARM64_ABI_KIND_FLOAT, size = 4;
  else return 0;
  return abi.arg[arg].kind == kind && abi.arg[arg].size == size &&
         abi.arg[arg].cls == (kind == LJ_ARM64_ABI_KIND_FLOAT ?
                              LJ_ARM64_ABI_FPR : LJ_ARM64_ABI_GPR);
}

static int locate_read(ScalarFixture *fx, IRRef callref, ScalarRead *out)
{
  IRRef args[32], r, lookup = 0, value = 0, carrier = 0, root1, root2;
  IRRef statusref, argref, xsave;
  IRIns call = ir_load_acq(&fx->ir[callref]);
  MSize pre, read, after, xsnap, entry;
  IRIns helper, vload, tail, head, status;
  BCIns sourceins, bytecode;
  const BCIns *readpc, *callpc, *afterpc;
  IRType result_type;
  unsigned nargs, arg = fx->arg;
  unsigned candidates = 0;
  memset(out, 0, sizeof(*out));
  if (call.o != IR_CALLXS || arg >= 32 ||
      (nargs = carg_values(fx, callref, args, 31)) <= arg) return 0;
  argref = args[arg];
  if (!scalar_abi_arg_matches(fx, callref, arg, argref)) return 0;
  for (r = REF_FIRST; r+2u < callref; r++) {
    IRIns candidate = ir_load_acq(&fx->ir[r]);
    IRIns result;
    if (candidate.o != IR_CALLS ||
        (candidate.op2 != IRCALL_lj_tab_getscalar_rooted_try &&
         candidate.op2 != IRCALL_lj_tab_gettv_rooted_hit_try)) continue;
    result = ir_load_acq(&fx->ir[r+2u]);
    if (result.o != IR_VLOAD || !irt_isguard(result.t)) continue;
    if (carrier_from_value(fx, argref, r+2u, irt_type(result.t))) {
      lookup = r; value = r+2u; carrier = argref; candidates++;
    }
  }
  if (candidates != 1 || value < REF_FIRST+2u) return 0;
  helper = ir_load_acq(&fx->ir[lookup]);
  vload = ir_load_acq(&fx->ir[value]);
  statusref = lookup+1u;
  status = ir_load_acq(&fx->ir[statusref]);
  if (status.o != IR_NE || status.t.irt != (IRT_INT|IRT_GUARD) ||
      !((status.op1 == lookup && irref_isk(status.op2)) ||
        (status.op2 == lookup && irref_isk(status.op1)))) return 0;
  {
    IRRef zero = status.op1 == lookup ? status.op2 : status.op1;
    if (zero < trace_nk_acq(fx->owner) ||
        ir_load_acq(&fx->ir[zero]).o != IR_KINT ||
        ir_load_acq(&fx->ir[zero]).i != 0) return 0;
  }
  if (helper.op1 < REF_FIRST || helper.op1 >= lookup) return 0;
  tail = ir_load_acq(&fx->ir[helper.op1]);
  if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
      tail.op1 < REF_FIRST || tail.op1 >= helper.op1 ||
      tail.op2 < REF_FIRST || tail.op2 >= lookup) return 0;
  root1 = tail.op2;
  head = ir_load_acq(&fx->ir[tail.op1]);
  if (head.o != IR_CARG || head.t.irt != IRT_NIL ||
      head.op1 != root1 || head.op2 < REF_FIRST || head.op2 >= lookup ||
      vload.op1 != root1 || vload.op2) return 0;
  root2 = head.op2;
  if (ir_load_acq(&fx->ir[root1]).o != IR_TMPREF ||
      ir_load_acq(&fx->ir[root1]).t.irt != IRT_PGC ||
      ir_load_acq(&fx->ir[root1]).op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
      ir_load_acq(&fx->ir[root2]).o != IR_TMPREF ||
      ir_load_acq(&fx->ir[root2]).t.irt != IRT_PGC ||
      ir_load_acq(&fx->ir[root2]).op2 != IRTMPREF_IN2) return 0;
  if (!snap_before(fx, statusref, &pre) || !snap_for_ref(fx, value, &read) ||
      !snap_after(fx, value, &after) || !snap_before(fx, callref, &xsnap))
    return 0;
  readpc = footer_pc(fx, pre);
  if (!readpc || footer_pc(fx, read) != readpc ||
      !snapshots_equal(fx, pre, read) || snap_ref_acq(&fx->snap[read]) != value)
    return 0;
  bytecode = loadbc(readpc);
  if (bc_op(bytecode) != BC_TGETS && bc_op(bytecode) != BC_TGETV &&
      bc_op(bytecode) != BC_TGETB) return 0;
  result_type = irt_type(vload.t);
  if (result_type != IRT_INT && result_type != IRT_NUM &&
      result_type != IRT_TRUE && result_type != IRT_FALSE) return 0;
  {
    IRRef table = ir_load_acq(&fx->ir[root1]).op1;
    IRRef key = ir_load_acq(&fx->ir[root2]).op1;
    IRIns t = ir_load_acq(&fx->ir[table]);
    if (table < REF_FIRST || (t.o != IR_SLOAD && t.o != IR_VLOAD) ||
        irt_type(t.t) != IRT_TAB || !irt_isguard(t.t) ||
        (t.o == IR_SLOAD && t.op2 != IRSLOAD_TYPECHECK) || key >= lookup)
      return 0;
    if (bc_op(bytecode) == BC_TGETV) {
      if (bc_c(bytecode) >= fx->pt->framesize) return 0;
    } else if (bc_op(bytecode) == BC_TGETS) {
      if (bc_c(bytecode) >= fx->pt->sizekgc ||
          ir_load_acq(&fx->ir[key]).o != IR_KGC ||
          irt_type(ir_load_acq(&fx->ir[key]).t) != IRT_STR ||
          ir_kgc_load_acq(&fx->ir[key]) !=
            proto_kgc_acq(fx->pt, ~(int32_t)bc_c(bytecode))) return 0;
    } else if (ir_load_acq(&fx->ir[key]).o != IR_KINT ||
               ir_load_acq(&fx->ir[key]).i != (int32_t)bc_c(bytecode)) {
      return 0;
    }
    if (bc_b(bytecode) >= fx->pt->framesize || bc_a(bytecode) >= fx->pt->framesize)
      return 0;
    if (bc_op(bytecode) == BC_TGETV && bc_c(bytecode) >= fx->pt->framesize)
      return 0;
  }
  afterpc = footer_pc(fx, after);
  if (!afterpc || afterpc <= readpc || !safe_setup_range(readpc, afterpc)) return 0;
  out->result_slot = (BCReg)(1u+LJ_FR2+bc_a(bytecode));
  if (!snapshot_slot(fx, after, out->result_slot, &out->result_entry, &r))
    return 0;
  if (result_type == IRT_TRUE || result_type == IRT_FALSE) {
    IRRef truth = result_type == IRT_TRUE ? REF_TRUE : REF_FALSE;
    if (r != truth) return 0;
  } else if (r != value) return 0;
  xsave = 0;
  for (r = callref; r > REF_FIRST;) {
    IRRef p = --r;
    if (ir_load_acq(&fx->ir[p]).o == IR_XSAVE) { xsave = p; break; }
  }
  if (!xsave || !snap_for_ref(fx, xsave, &xsnap)) return 0;
  {
    IRIns mark = ir_load_acq(&fx->ir[xsave]);
    uint32_t pc_offset = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
    if (pc_offset == 0 || pc_offset >= fx->pt->sizebc) return 0;
    callpc = &proto_bc(fx->pt)[pc_offset-1u];
    sourceins = loadbc(callpc);
  }
  if (bc_op(sourceins) != BC_CALL || bc_c(sourceins) == 0 ||
      (unsigned)bc_c(sourceins)-1u != nargs) return 0;
  {
    BCReg base = (BCReg)(bc_a(sourceins)+2u*(1u+LJ_FR2)+1u);
    BCReg argslot = (BCReg)(base+arg);
    IRRef mapped, staged_expected = carrier;
    if (argslot >= UINT8_MAX ||
        !snapshot_slot(fx, xsnap, argslot, &entry, &mapped))
      return 0;
    if (result_type == IRT_TRUE || result_type == IRT_FALSE)
      staged_expected = result_type == IRT_TRUE ? REF_TRUE : REF_FALSE;
    if (mapped != carrier && mapped != value && mapped != staged_expected)
      return 0;
    out->arg_slot = argslot;
    out->arg_entry = entry;
  }
  {
    IRRef fnref = call.op2, cdref, cr;
    IRIns fp = ir_load_acq(&fx->ir[fnref]);
    if (fp.o != IR_FLOAD || fp.op2 != IRFL_CDATA_PTR ||
        fp.op1 < REF_FIRST || fp.op1 >= fnref) return 0;
    cdref = fp.op1;
    for (cr = REF_FIRST; cr < callref; cr++) {
      IRIns ci = ir_load_acq(&fx->ir[cr]);
      if (ci.o == IR_FLOAD && ci.op1 == cdref &&
          ci.op2 == IRFL_CDATA_CTYPEID) break;
    }
    if (cr >= callref) return 0;
    for (r = cr+1u; r < callref; r++) {
      IRIns eq = ir_load_acq(&fx->ir[r]);
      IRRef k;
      if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
          (eq.op1 != cr && eq.op2 != cr)) continue;
      k = eq.op1 == cr ? eq.op2 : eq.op1;
      if (k < trace_nk_acq(fx->owner) ||
          ir_load_acq(&fx->ir[k]).o != IR_KINT) continue;
      out->ctype = cr; out->ctype_guard = r; out->ctype_const = k;
      break;
    }
    if (!out->ctype ||
        ir_load_acq(&fx->ir[out->ctype_const]).i != (int32_t)global_ctype(fx->L, fx->fn)) return 0;
  }
  {
    IRRef table = ir_load_acq(&fx->ir[root1]).op1;
    IRRef key = ir_load_acq(&fx->ir[root2]).op1;
    BCReg ts = (BCReg)(1u+LJ_FR2+bc_b(bytecode));
    IRRef mapped;
    if (ts >= UINT8_MAX ||
        !snapshot_slot(fx, pre, ts, &out->table_entry, &mapped) ||
        mapped != table || !snapshot_slot(fx, read, ts, NULL, &mapped) ||
        mapped != table) return 0;
    out->table_slot = ts;
    if (bc_op(bytecode) == BC_TGETV) {
      BCReg ks = (BCReg)(1u+LJ_FR2+bc_c(bytecode));
      if (ks >= UINT8_MAX ||
          !snapshot_slot(fx, pre, ks, &out->key_entry, &mapped) ||
          mapped != key || !snapshot_slot(fx, read, ks, NULL, &mapped) ||
          mapped != key) return 0;
      out->key_slot = ks;
    }
  }
  out->call = callref;
  out->lookup = lookup;
  out->value = value;
  out->root1 = root1;
  out->root2 = root2;
  out->status = statusref;
  out->carrier = carrier;
  out->xsave = xsave;
  out->pre = pre;
  out->read = read;
  out->after = after;
  out->xsnap = xsnap;
  out->arg = arg;
  out->result_type = result_type;
  out->read_pc = readpc;
  out->call_pc = callpc;
  return 1;
}

static void locate_read_for_call(ScalarFixture *fx)
{
  IRRef r;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_CALLXS &&
        locate_read(fx, r, &fx->read)) return;
  fprintf(stderr, "no scalar table source for %s arg %u\n", fx->runner, fx->arg);
  assert(0);
}

static void reset_fixture(ScalarFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner), full = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)full*sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
         (size_t)(full-nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner), (size_t)fx->nsnap*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)fx->nsnapmap*sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  fx->nins = semantic_nins(fx->owner);
  locate_read_for_call(fx);
}

static void free_fixture(ScalarFixture *fx)
{ free(fx->snapmap); free(fx->snap); free(fx->ir); memset(fx, 0, sizeof(*fx)); }

static JSave enter_semantic(ScalarFixture *fx)
{
  jit_State *J = fx->J;
  JSave s = { J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace };
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace+1u].tv.u64 = 0;
  J->curfinal = NULL; J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner); J->loopref = 0;
  for (IRRef r = REF_FIRST; r < fx->nins; r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) { J->loopref = r; break; }
  assert(J->loopref != 0);
  J->parent = 0; J->exitno = 0; J->baseslot = 1u+LJ_FR2;
  J->framedepth = 0; J->retdepth = 0; J->ktrace = fx->ktrace;
  return s;
}

static void leave_semantic(jit_State *J, JSave *s)
{
  J->curfinal=s->curfinal; J->pt=s->pt; J->startpc=s->startpc;
  J->loopref=s->loopref; J->parent=s->parent; J->exitno=s->exitno;
  J->baseslot=s->baseslot; J->framedepth=s->framedepth;
  J->retdepth=s->retdepth; J->ktrace=s->ktrace;
}

static LJArm64PostRAView postra_view(ScalarFixture *fx)
{
  LJArm64PostRAView v = {0};
  v.ir=fx->ir; v.owner=&fx->view_owner; v.cts=fx->cts;
  v.snap=fx->snap; v.snapmap=fx->snapmap;
  v.proto_bc=proto_bc(fx->pt); v.proto=fx->pt;
  v.startpc=trace_startpc_acq(fx->owner);
  v.nins=trace_nins_acq(fx->owner); v.nk=trace_nk_acq(fx->owner);
  v.nsnap=fx->nsnap; v.nsnapmap=fx->nsnapmap;
  v.spadjust=trace_spadjust_acq(fx->owner);
  v.proto_sizebc=fx->pt->sizebc; v.proto_numparams=fx->pt->numparams;
  v.root_topslot=trace_topslot_acq(fx->owner);
  v.startins=trace_startins_acq(fx->owner);
  return v;
}

static int check_semantic(ScalarFixture *fx)
{
  uint32_t *pc = (uint32_t *)trace_startpc_acq(fx->owner);
  uint32_t saved = la_load32_acq(pc);
  JSave s;
  int ok;
  la_store32_rel(pc, fx->owner->startins);
  s = enter_semantic(fx);
  ok = lj_asm_arm64_ir_admit(fx->J, &fx->semantic, NULL);
  leave_semantic(fx->J, &s);
  la_store32_rel(pc, saved);
  return ok;
}

static int check_postra(ScalarFixture *fx)
{
  LJArm64PostRAView v = postra_view(fx);
  return lj_asm_arm64_postra_admit(&v, NULL);
}

static IRRef other_scalar_value(ScalarFixture *fx, IRRef before, IRRef skip)
{
  IRType want;
  if (skip < REF_FIRST || skip >= before) return 0;
  want = irt_type(ir_load_acq(&fx->ir[skip]).t);
  for (IRRef r = REF_FIRST; r < before; r++) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (r == skip) continue;
    if (in.o == IR_VLOAD && irt_type(in.t) == want && irt_isguard(in.t))
      return r;
  }
  for (IRRef r = trace_nk_acq(fx->owner); r < REF_TRUE; r++) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if ((want == IRT_INT && in.o == IR_KINT) ||
        (want == IRT_NUM && in.o == IR_KNUM && in.t.irt == IRT_NUM)) return r;
    if (in.o == IR_KNUM || in.o == IR_KINT64 ||
        (LJ_GC64 && (in.o == IR_KGC || in.o == IR_KPTR ||
                     in.o == IR_KKPTR))) r++;
  }
  return 0;
}

static void mutate(ScalarFixture *fx, enum Mutation m)
{
  ScalarRead *c = &fx->read;
  IRRef alt = other_scalar_value(fx, c->call, c->value);
  switch (m) {
  case M_HELPER: {
    IRRef key = fx->ir[c->root2].op1;
    int generic_table_key = fx->ir[c->lookup].op2 ==
      IRCALL_lj_tab_gettv_rooted_hit_try &&
      bc_op(loadbc(c->read_pc)) == BC_TGETV &&
      strcmp(fx->runner, "__table_scalar_run_v_tablekey_double") == 0 &&
      irt_type(ir_load_acq(&fx->ir[key]).t) == IRT_TAB;
    fx->ir[c->lookup].op2 = generic_table_key ?
      IRCALL_lj_tab_getscalar_rooted_try :
      IRCALL_lj_tab_getnil_nometa_rooted_try;
    break;
  }
  case M_STATUS:
    fx->ir[c->status].o = IR_EQ; break;
  case M_ROOT1_MODE: fx->ir[c->root1].op2 = IRTMPREF_IN1; break;
  case M_ROOT2_MODE: fx->ir[c->root2].op2 = IRTMPREF_IN1; break;
  case M_CARG_ROOT: {
    IRRef tail = fx->ir[c->lookup].op1;
    assert(fx->ir[tail].o == IR_CARG);
    fx->ir[tail].op2 = c->root2;
    break;
  }
  case M_TABLE_SOURCE:
    fx->ir[c->root1].op1 = fx->ir[c->root2].op1; break;
  case M_VLOAD_BASE: fx->ir[c->value].op1 = c->root2; break;
  case M_VLOAD_OFFSET: fx->ir[c->value].op2 = 1; break;
  case M_VLOAD_TYPE: fx->ir[c->value].t.irt = IRT_TAB|IRT_GUARD; break;
  case M_VLOAD_GUARD: fx->ir[c->value].t.irt &= (IRType)~IRT_GUARD; break;
  case M_READ_PC: set_footer_pc(fx, c->pre, c->read_pc+1u); break;
  case M_READ_MAP:
    fx->snapmap[c->table_entry] = snap_setref(
      fx->snapmap[c->table_entry], fx->ir[c->root2].op1); break;
  case M_RESULT_MAP:
    if (c->result_type == IRT_TRUE || c->result_type == IRT_FALSE)
      alt = c->result_type == IRT_TRUE ? REF_FALSE : REF_TRUE;
    if (alt) fx->snapmap[c->result_entry] = snap_setref(
      fx->snapmap[c->result_entry], alt);
    else fx->snapmap[c->result_entry] |= SNAP_NORESTORE;
    break;
  case M_RESULT_PC: set_footer_pc(fx, c->after, c->read_pc); break;
  case M_CALLARG_SOURCE: {
    IRRef node = carg_node(fx, c->call, c->arg);
    alt = other_scalar_value(fx, c->call, c->carrier);
    assert(node != 0 && alt != 0);
    if (c->arg == 0) fx->ir[node].op1 = alt;
    else fx->ir[node].op2 = alt;
    break;
  }
  case M_XSAVE_SOURCE:
    if (c->result_type == IRT_TRUE || c->result_type == IRT_FALSE)
      alt = c->result_type == IRT_TRUE ? REF_FALSE : REF_TRUE;
    assert(alt != 0);
    fx->snapmap[c->arg_entry] = snap_setref(fx->snapmap[c->arg_entry], alt);
    break;
  case M_XSAVE_NORESTORE:
    fx->snapmap[c->arg_entry] |= SNAP_NORESTORE; break;
  case M_XSAVE_TRUNCATED:
    assert(fx->snap[c->xsnap].nslots > c->arg_slot);
    fx->snap[c->xsnap].nslots = c->arg_slot; break;
  case M_CONV_SOURCE:
    assert(fx->ir[c->carrier].o == IR_CONV);
    fx->ir[c->carrier].op1 = alt; break;
  case M_CONV_MODE:
    assert(fx->ir[c->carrier].o == IR_CONV);
    fx->ir[c->carrier].op2 ^= IRCONV_CHECK; break;
  case M_ABI_TYPE: {
    const char *other = strcmp(fx->fn, "__table_scalar_fn_double") == 0 ?
      "__table_scalar_fn_i32" : "__table_scalar_fn_double";
    fx->ir[c->ctype_const].i = (int32_t)global_ctype(fx->L, other);
    break;
  }
  case M_BOOL_TAG:
    assert(c->result_type == IRT_TRUE || c->result_type == IRT_FALSE);
    fx->ir[c->value].t.irt =
      (IRType)((c->result_type == IRT_TRUE ? IRT_FALSE : IRT_TRUE)|IRT_GUARD);
    break;
  case M_BOOL_VALUE: {
    IRRef node = carg_node(fx, c->call, c->arg);
    IRRef kref = 0;
    assert(c->result_type == IRT_TRUE || c->result_type == IRT_FALSE);
    for (IRRef k = trace_nk_acq(fx->owner); k < REF_TRUE; k++) {
      IRIns constant = ir_load_acq(&fx->ir[k]);
      if (constant.o == IR_KINT && constant.t.irt == IRT_INT &&
          constant.i == (c->result_type == IRT_TRUE ? 0 : 1)) {
        kref = k; break;
      }
      if (constant.o == IR_KNUM || constant.o == IR_KINT64 ||
          (LJ_GC64 && (constant.o == IR_KGC || constant.o == IR_KPTR ||
                       constant.o == IR_KKPTR))) k++;
    }
    assert(node != 0 && kref != 0);
    if (c->arg == 0) fx->ir[node].op1 = kref;
    else fx->ir[node].op2 = kref;
    break;
  }
  default: assert(!"post-RA-only mutation in semantic path");
  }
}

static void expect_both(ScalarFixture *fx, enum Mutation m, const char *name)
{
  int ok;
  reset_fixture(fx); mutate(fx, m); ok = check_semantic(fx);
  if (ok) fprintf(stderr, "table scalar semantic accepted %s\n", name);
  assert(!ok);
  semantic_rejections++;
  reset_fixture(fx); mutate(fx, m); ok = check_postra(fx);
  if (ok) fprintf(stderr, "table scalar post-RA accepted %s\n", name);
  assert(!ok);
  allocated_rejections++;
}

static void expect_postra(ScalarFixture *fx, enum Mutation m, const char *name)
{
  ScalarRead *c;
  int ok;
  reset_fixture(fx); c = &fx->read;
  if (m == M_ROOT_ALIAS) fx->ir[c->root2].r = fx->ir[c->root1].r;
  else if (m == M_ROOT_SPILL) fx->ir[c->root1].s = SPS_FIRST;
  else if (m == M_VALUE_SPILL_HALF)
    fx->ir[c->value].s = (uint8_t)(SPS_FIRST+1u);
  else if (m == M_VALUE_SPILL_OUTSIDE) {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(cap < SPS_LIMIT); fx->ir[c->value].s = (uint8_t)cap;
  } else if (m == M_VALUE_WRONG_BANK) {
    if (c->result_type == IRT_NUM) fx->ir[c->value].r = RID_X0;
    else fx->ir[c->value].r = RID_D0;
  } else if (m == M_CALL_RESULT_BANK) {
    if (irt_isfp(fx->ir[c->call].t)) fx->ir[c->call].r = RID_RET;
    else fx->ir[c->call].r = RID_FPRET;
  } else assert(!"unknown allocated-view mutation");
  ok = check_postra(fx);
  if (ok) fprintf(stderr, "table scalar post-RA accepted %s\n", name);
  assert(!ok);
  allocated_rejections++;
}

static void run_lua(lua_State *L, const char *src)
{
  int status = luaL_dostring(L, src);
  if (status != LUA_OK) {
    fprintf(stderr, "table scalar admission setup: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer call_count(lua_State *L, const char *name)
{
  lua_Integer n;
  lua_getglobal(L, name);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1));
  n = lua_tointeger(L, -1); lua_pop(L, 1); return n;
}

static void run_case(lua_State *L, const char *runner, const char *fn,
                     int mode, int key, lua_Number expected,
                     const char *counter)
{
  lua_getglobal(L, "__table_scalar_configure");
  lua_pushinteger(L, mode);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
  lua_getglobal(L, "__table_scalar_driver");
  lua_getglobal(L, runner); lua_getglobal(L, "__table_scalar_registry");
  lua_getglobal(L, fn); lua_pushinteger(L, key); lua_pushinteger(L, 32);
  if (lua_pcall(L, 5, 1, 0) != LUA_OK) {
    fprintf(stderr, "table scalar runner %s failed: %s\n", runner,
            lua_tostring(L, -1)); assert(0);
  }
  assert(lua_isnumber(L, -1) && lua_tonumber(L, -1) == expected);
  lua_pop(L, 1);
  assert(call_count(L, counter) == 32);
}

static void check_original(ScalarFixture *fx)
{
  uint32_t *pc = (uint32_t *)trace_startpc_acq(fx->owner);
  uint32_t saved = la_load32_acq(pc);
  JSave s;
  LJArm64PostRAView v;
  reset_fixture(fx); la_store32_rel(pc, fx->owner->startins);
  s = enter_semantic(fx);
  assert(lj_asm_arm64_ir_admit(fx->J, &fx->semantic, NULL));
  leave_semantic(fx->J, &s); la_store32_rel(pc, saved);
  reset_fixture(fx); v = postra_view(fx);
  assert(lj_asm_arm64_postra_admit(&v, NULL));
  positive_cases++;
}

static void capture_fixture(lua_State *L, jit_State *J, ScalarFixture *fx,
                            const char *runner, const char *fn,
                            unsigned arg)
{
  fx->L = L; fx->J = J; fx->runner = runner; fx->fn = fn; fx->arg = arg;
  fx->pt = global_proto(L, runner); fx->owner = find_trace(J, fx->pt);
  if (!fx->owner) fprintf(stderr, "missing table scalar trace: %s\n", runner);
  assert(fx->owner != NULL);
  fx->cts = ctype_ctsG(G(L)); fx->ktrace = find_ktrace(fx->owner);
  fx->nsnap = trace_nsnap_acq(fx->owner);
  fx->nsnapmap = trace_nsnapmap_acq(fx->owner);
  assert(fx->cts && fx->ktrace && fx->nsnap && fx->nsnapmap);
  fx->ir = (IRIns *)calloc(trace_nins_acq(fx->owner), sizeof(IRIns));
  fx->snap = (SnapShot *)calloc(fx->nsnap, sizeof(SnapShot));
  fx->snapmap = (SnapEntry *)calloc(fx->nsnapmap, sizeof(SnapEntry));
  assert(fx->ir && fx->snap && fx->snapmap);
  reset_fixture(fx);
}

static void test_fixture(lua_State *L, jit_State *J, ScalarFixture *fx,
                         const char *runner, const char *fn, unsigned arg,
                         int mode, int key, lua_Number expected,
                         const char *counter, int boolean_case)
{
  run_lua(L, "jit.flush()");
  run_case(L, runner, fn, mode, key, expected, counter);
  memset(fx, 0, sizeof(*fx));
  capture_fixture(L, J, fx, runner, fn, arg);
  check_original(fx);
  expect_both(fx, M_ABI_TYPE, "CALLXS cdata function type mismatch");
  expect_both(fx, M_HELPER, "wrong rooted scalar helper");
  expect_both(fx, M_STATUS, "wrong helper status");
  expect_both(fx, M_ROOT1_MODE, "wrong output-root mode");
  expect_both(fx, M_ROOT2_MODE, "wrong key-root mode");
  expect_both(fx, M_CARG_ROOT, "helper output CARG uses key root");
  expect_both(fx, M_TABLE_SOURCE, "lookup root uses key as table");
  expect_both(fx, M_VLOAD_BASE, "result VLOAD uses wrong root");
  expect_both(fx, M_VLOAD_OFFSET, "result VLOAD uses nonzero offset");
  expect_both(fx, M_VLOAD_TYPE, "result VLOAD has wrong tag");
  expect_both(fx, M_VLOAD_GUARD, "result VLOAD is unguarded");
  expect_both(fx, M_READ_PC, "retry snapshot skips source table read");
  expect_both(fx, M_READ_MAP, "pre-read snapshot has wrong table source");
  expect_both(fx, M_RESULT_MAP, "later snapshot loses table result");
  expect_both(fx, M_RESULT_PC, "later snapshot resumes at wrong PC");
  if (!boolean_case && other_scalar_value(fx, fx->read.call,
                                           fx->read.carrier) != 0)
    expect_both(fx, M_CALLARG_SOURCE, "call argument comes from another value");
  expect_both(fx, M_XSAVE_SOURCE, "XSAVE argument has another source");
  expect_both(fx, M_XSAVE_NORESTORE, "XSAVE argument is not restored");
  expect_both(fx, M_XSAVE_TRUNCATED, "XSAVE argument span truncated");
  if (boolean_case) {
    expect_both(fx, M_BOOL_TAG, "boolean VLOAD tag mismatch");
    expect_both(fx, M_BOOL_VALUE, "boolean call argument constant mismatch");
  } else if (fx->ir[fx->read.carrier].o == IR_CONV) {
    expect_both(fx, M_CONV_SOURCE, "ABI conversion reads another value");
    expect_both(fx, M_CONV_MODE, "ABI conversion mode mismatch");
  }
  expect_postra(fx, M_ROOT_ALIAS, "table/key scratch register alias");
  expect_postra(fx, M_ROOT_SPILL, "root scratch spills");
  if (fx->read.result_type == IRT_INT || fx->read.result_type == IRT_NUM) {
    if (fx->read.result_type == IRT_NUM)
      expect_postra(fx, M_VALUE_SPILL_HALF, "scalar result half-slot spill");
    expect_postra(fx, M_VALUE_SPILL_OUTSIDE, "scalar result spill outside frame");
    expect_postra(fx, M_VALUE_WRONG_BANK, "scalar result wrong register bank");
  }
  expect_postra(fx, M_CALL_RESULT_BANK, "CALLXS result wrong register bank");
  free_fixture(fx);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  ScalarFixture fx;
  const char *lib = getenv("LJ_M7_FFI_TABLE_SCALAR_ARGS_SO");
  assert(lib && *lib);
  run_lua(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_table_scalar_configure(int32_t); int32_t lj_table_scalar_i32_count(void); int32_t lj_table_scalar_double_count(void); int32_t lj_table_scalar_float_count(void); int32_t lj_table_scalar_bool_count(void); int32_t lj_table_scalar_i32_9_count(void); int32_t lj_table_scalar_mixed11_count(void); int32_t lj_table_scalar_total_count(void);]]\n"
    "ffi.cdef[[int32_t lj_table_scalar_i32(int32_t); int32_t lj_table_scalar_bool_i32(int32_t); double lj_table_scalar_double(double); float lj_table_scalar_float(float); int32_t lj_table_scalar_i32_9(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t); double lj_table_scalar_mixed11(int32_t,double,int32_t,float,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_SCALAR_ARGS_SO')))\n"
    "local i32type=ffi.typeof('int32_t(*)(int32_t)'); local dbtype=ffi.typeof('double(*)(double)'); local fltype=ffi.typeof('float(*)(float)')\n"
    "_G.__table_scalar_fn_i32=ffi.cast(i32type,lib.lj_table_scalar_i32); _G.__table_scalar_fn_double=ffi.cast(dbtype,lib.lj_table_scalar_double); _G.__table_scalar_fn_float=ffi.cast(fltype,lib.lj_table_scalar_float); _G.__table_scalar_fn_bool=ffi.cast(i32type,lib.lj_table_scalar_bool_i32); _G.__table_scalar_fn_i32_9=ffi.cast(ffi.typeof('int32_t(*)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t)'),lib.lj_table_scalar_i32_9); _G.__table_scalar_fn_mixed11=ffi.cast(ffi.typeof('double(*)(int32_t,double,int32_t,float,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t)'),lib.lj_table_scalar_mixed11)\n"
    "local identity={}; local registry={s={value=-17},v={[2]=2.5},list={1.5},bool={value=true,false_value=false},tablekey=identity,root={branch={leaf={}}}}; registry.v[identity]=5.5\n"
    "local leaf=registry.root.branch.leaf; for i=0,8 do leaf['a'..i]=i+1 end; leaf.d0=1.25; leaf.f0=2.5\n"
    "_G.__table_scalar_registry=registry; function _G.__table_scalar_configure(mode) lib.lj_table_scalar_configure(mode) end\n"
    "_G.__table_scalar_i32_count=lib.lj_table_scalar_i32_count; _G.__table_scalar_double_count=lib.lj_table_scalar_double_count; _G.__table_scalar_float_count=lib.lj_table_scalar_float_count; _G.__table_scalar_bool_count=lib.lj_table_scalar_bool_count; _G.__table_scalar_i32_9_count=lib.lj_table_scalar_i32_9_count; _G.__table_scalar_mixed11_count=lib.lj_table_scalar_mixed11_count; _G.__table_scalar_total_count=lib.lj_table_scalar_total_count\n"
    "_G.__table_scalar_ready=threading.channel(1); _G.__table_scalar_stop=threading.channel(1); _G.__table_scalar_peer=threading.spawn(function(reg,ready,stop) assert(ready:send(true,5)); local _,ok=stop:recv(30); assert(ok); return reg~=nil end,registry,__table_scalar_ready,__table_scalar_stop); local _,ok=__table_scalar_ready:recv(5); assert(ok)\n"
    "function _G.__table_scalar_run_s_i32(reg,fn,key,n) local t=reg.s; local sum=0.5; for i=1,n do sum=sum+fn(t.value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_v_double(reg,fn,key,n) local t=reg.v; local sum=0.5; for i=1,n do sum=sum+fn(t[key]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_v_tablekey_double(reg,fn,key,n) local t=reg.v; local k=reg.tablekey; local sum=0.5; for i=1,n do sum=sum+fn(t[k]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_b_float(reg,fn,key,n) local t=reg.list; local sum=0.5; for i=1,n do sum=sum+fn(t[1]) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_bool_i32(reg,fn,key,n) local t=reg.bool; local sum=0.5; for i=1,n do sum=sum+fn(t.value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_bool_false_i32(reg,fn,key,n) local t=reg.bool; local sum=0.5; for i=1,n do sum=sum+fn(t.false_value) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_nested9(reg,fn,key,n) local root=reg.root; local sum=0.5; for i=1,n do local t=root.branch.leaf; sum=sum+fn(t.a0,t.a1,t.a2,t.a3,t.a4,t.a5,t.a6,t.a7,t.a8) end; return sum-0.5 end\n"
    "function _G.__table_scalar_run_mixed11(reg,fn,key,n) local root=reg.root; local sum=0.5; for i=1,n do local t=root.branch.leaf; sum=sum+fn(t.a0,t.d0,t.a1,t.f0,t.a2,t.a3,t.a4,t.a5,t.a6,t.a7,t.a8) end; return sum-0.5 end\n"
    "function _G.__table_scalar_driver(run,reg,fn,key,n) local result=run(reg,fn,key,n); return result end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");

  test_fixture(L,J,&fx,"__table_scalar_run_s_i32","__table_scalar_fn_i32",0,0,2,-544.0,"__table_scalar_i32_count",0);
  test_fixture(L,J,&fx,"__table_scalar_run_v_double","__table_scalar_fn_double",0,3,2,80.0,"__table_scalar_double_count",0);
  test_fixture(L,J,&fx,"__table_scalar_run_v_tablekey_double","__table_scalar_fn_double",0,10,2,176.0,"__table_scalar_double_count",0);
  test_fixture(L,J,&fx,"__table_scalar_run_b_float","__table_scalar_fn_float",0,4,2,48.0,"__table_scalar_float_count",0);
  test_fixture(L,J,&fx,"__table_scalar_run_bool_i32","__table_scalar_fn_bool",0,2,2,32.0,"__table_scalar_bool_count",1);
  test_fixture(L,J,&fx,"__table_scalar_run_bool_false_i32","__table_scalar_fn_bool",0,11,2,0.0,"__table_scalar_bool_count",1);
  test_fixture(L,J,&fx,"__table_scalar_run_nested9","__table_scalar_fn_i32_9",0,0,2,1440.0,"__table_scalar_i32_9_count",0);
  test_fixture(L,J,&fx,"__table_scalar_run_mixed11","__table_scalar_fn_mixed11",3,0,2,1560.0,"__table_scalar_mixed11_count",0);
  run_lua(L,"assert(__table_scalar_stop:send(true,5)); local ok,result=__table_scalar_peer:join(5); assert(ok and result)");
  lua_close(L);
  printf("t-arm64-jit-table-scalar-args-admission OK: %u positives, "
         "%u semantic and %u allocated rejections\n",
         positive_cases, semantic_rejections, allocated_rejections);
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-scalar-args-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}

#endif
