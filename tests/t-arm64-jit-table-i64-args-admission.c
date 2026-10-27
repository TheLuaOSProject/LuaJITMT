/* Independent semantic and allocated-view proofs for table-rooted I64 CALLXS
** arguments. The table value remains a GC root in XSAVE; FLOAD bits are not. */

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
#error "table I64 CALLXS admission requires ARM64 CALLXS admission"
#endif

typedef struct I64Read {
  IRRef lookup, root1, root2, status, value, payload, ctype, guard;
  IRRef call, xsave;
  MSize pre_snap, read_snap, after_snap, xsave_snap;
  BCReg result_slot, call_arg_slot;
  MSize result_entry, xsave_entry;
  const BCIns *read_pc, *call_pc;
  unsigned arg;
} I64Read;

typedef struct I64Fixture {
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
  IRRef calls[8], xsaves[8];
  unsigned ncalls, target, arg_index;
  I64Read read;
} I64Fixture;

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
  M_HELPER, M_STATUS, M_ROOT1_MODE, M_ROOT2_MODE, M_LOOKUP_CARG,
  M_RECEIVER_GUARD,
  M_VLOAD_BASE, M_VLOAD_OFFSET, M_VLOAD_TYPE, M_VLOAD_GUARD,
  M_CTYPE_SOURCE, M_CTYPE_WIDTH, M_CTYPE_GUARD,
  M_PAYLOAD_SOURCE, M_PAYLOAD_WIDTH, M_PAYLOAD_SIGN,
  M_CARG_OTHER, M_READ_PC, M_POSTREAD_PC, M_RESULT_SLOT,
  M_XSAVE_OTHER, M_XSAVE_NORESTORE, M_XSAVE_TRUNCATED,
  M_ROOT_ALIAS, M_ROOT_SPILL, M_VLOAD_SPILL_HALF, M_VLOAD_SPILL_OUTSIDE,
  M_PAYLOAD_SPILL_HALF, M_PAYLOAD_SPILL_OUTSIDE
};

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

static unsigned op_count(const GCtrace *T, IROp op)
{
  IRRef r;
  unsigned n = 0;
  const IRIns *ir = trace_ir_acq(T);
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++)
    if (ir_load_acq(&ir[r]).o == op) n++;
  return n;
}

static unsigned read_count(const GCtrace *T)
{
  IRRef r;
  unsigned n = 0;
  const IRIns *ir = trace_ir_acq(T);
  for (r = REF_FIRST; r < trace_nins_acq((GCtrace *)T); r++)
    if (ir_load_acq(&ir[r]).o == IR_CALLS &&
        ir_load_acq(&ir[r]).op2 == IRCALL_lj_tab_gettv_rooted_hit_try) n++;
  return n;
}

static IRRef find_ktrace(const GCtrace *T)
{
  IRRef r, nk = trace_nk_acq((GCtrace *)T);
  const IRIns *ir = trace_ir_acq(T);
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
    unsigned calls, reads;
    if (!T || !trace_runnable_acq(T, no) || trace_root_acq(T) != 0 ||
        trace_startpt_acq(T) != pt ||
        bc_op(trace_startins_acq(T)) != BC_FORL) continue;
    calls = op_count(T, IR_CALLXS);
    reads = read_count(T);
    if (calls >= 2u && reads >= 2u)
      return T;
  }
  return NULL;
}

static int snapshot_slot(I64Fixture *fx, MSize sn, BCReg slot,
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

static int snap_for_ref(I64Fixture *fx, IRRef ref, MSize *out)
{
  MSize s;
  int found = 0;
  for (s = 0; s < fx->nsnap; s++) if (snap_ref_acq(&fx->snap[s]) == ref) {
    if (found++) return 0;
    *out = s;
  }
  return found == 1;
}

static int snap_before(I64Fixture *fx, IRRef ref, MSize *out)
{
  MSize s, best = UINT_MAX;
  IRRef bestref = 0;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at <= ref && at >= bestref) { best = s; bestref = at; }
  }
  if (best == UINT_MAX) return 0;
  *out = best;
  return 1;
}

static int snap_after(I64Fixture *fx, IRRef ref, MSize *out)
{
  MSize s, best = UINT_MAX;
  IRRef bestref = REF_DROP;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at > ref && at < bestref) { best = s; bestref = at; }
  }
  if (best == UINT_MAX) return 0;
  *out = best;
  return 1;
}

static const BCIns *footer_pc(I64Fixture *fx, MSize s)
{
  MSize ofs, nent;
  if (s >= fx->nsnap) return NULL;
  ofs = snap_mapofs_acq(&fx->snap[s]);
  nent = snap_nent_acq(&fx->snap[s]);
  if (ofs > fx->nsnapmap || nent > fx->nsnapmap-ofs ||
      1u+LJ_FR2 > fx->nsnapmap-ofs-nent) return NULL;
  return snap_pc(&fx->snapmap[ofs+nent]);
}

static void set_footer_pc(I64Fixture *fx, MSize sn, const BCIns *pc)
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

static unsigned carg_values(I64Fixture *fx, IRRef call, IRRef *values,
                            unsigned cap)
{
  IRRef rev[LJ_ARM64_ABI_MAX_ARGS];
  IRRef r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  while (r >= REF_FIRST && r < call) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) break;
    if (in.t.irt != IRT_NIL || n >= cap || in.op1 >= r || in.op2 >= r)
      return 0;
    rev[n++] = in.op2;
    r = in.op1;
  }
  if (r < REF_FIRST || r >= call || n == 0 || n > cap) return 0;
  values[0] = r;
  for (i = 0; i < n; i++) values[i+1u] = rev[n-i-1u];
  return n+1u;
}

static IRRef carg_node(I64Fixture *fx, IRRef call, unsigned arg)
{
  IRRef nodes[LJ_ARM64_ABI_MAX_ARGS], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0;
  while (r >= REF_FIRST && r < call) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) break;
    if (n >= LJ_ARM64_ABI_MAX_ARGS) return 0;
    nodes[n++] = r;
    r = in.op1;
  }
  if (arg > n) return 0;
  return arg == 0 ? nodes[n-1u] : nodes[n-arg];
}

static void set_carg(I64Fixture *fx, IRRef call, unsigned arg, IRRef value)
{
  IRRef node = carg_node(fx, call, arg);
  assert(node != 0);
  if (arg == 0) fx->ir[node].op1 = value;
  else fx->ir[node].op2 = value;
}

static int locate_read(I64Fixture *fx, IRRef call, unsigned arg, I64Read *out)
{
  IRRef values[LJ_ARM64_ABI_MAX_ARGS], value, payload, lookup, r;
  IRRef root1 = 0, root2 = 0, guard = 0, ctype = 0;
  IRIns carg, vload, cargtail, carghead, helper, statusins;
  MSize pre, read, after, xsave_snap, entry;
  BCIns src, callins;
  IRRef staged;
  uint32_t offset;
  unsigned n;
  memset(out, 0, sizeof(*out));
  n = carg_values(fx, call, values, LJ_ARM64_ABI_MAX_ARGS);
  if (arg >= n) return 0;
  value = values[arg];
  carg = ir_load_acq(&fx->ir[value]);
  if (carg.o != IR_FLOAD || carg.op2 != IRFL_CDATA_INT64 ||
      (carg.t.irt != IRT_I64 && carg.t.irt != IRT_U64)) return 0;
  payload = carg.op1;
  if (payload < REF_FIRST || payload >= value) return 0;
  vload = ir_load_acq(&fx->ir[payload]);
  if (vload.o != IR_VLOAD || vload.t.irt != (IRT_CDATA|IRT_GUARD) ||
      vload.op2 || payload < REF_FIRST+2u)
    return 0;
  lookup = payload-2u;
  helper = ir_load_acq(&fx->ir[lookup]);
  if (helper.o != IR_CALLS ||
      helper.op2 != IRCALL_lj_tab_gettv_rooted_hit_try ||
      helper.op1 < REF_FIRST || helper.op1 >= lookup) return 0;
  statusins = ir_load_acq(&fx->ir[lookup+1u]);
  if (statusins.o != IR_NE || statusins.t.irt != (IRT_INT|IRT_GUARD) ||
      !((statusins.op1 == lookup && statusins.op2 >= trace_nk_acq(fx->owner) &&
         fx->ir[statusins.op2].o == IR_KINT && fx->ir[statusins.op2].i == 0) ||
        (statusins.op2 == lookup && statusins.op1 >= trace_nk_acq(fx->owner) &&
         fx->ir[statusins.op1].o == IR_KINT && fx->ir[statusins.op1].i == 0)))
    return 0;
  cargtail = ir_load_acq(&fx->ir[helper.op1]);
  if (cargtail.o != IR_CARG || cargtail.op1 < REF_FIRST) return 0;
  root1 = cargtail.op2;
  if (vload.op1 != root1) return 0;
  carghead = ir_load_acq(&fx->ir[cargtail.op1]);
  if (carghead.o != IR_CARG || carghead.op1 != root1) return 0;
  root2 = carghead.op2;
  for (r = payload+1u; r < call; r++) {
    IRIns type = ir_load_acq(&fx->ir[r]);
    if (type.o == IR_FLOAD && type.op1 == payload &&
        type.op2 == IRFL_CDATA_CTYPEID) { ctype = r; break; }
  }
  if (!ctype) return 0;
  for (r = ctype+1u; r < call; r++) {
    IRIns eq = ir_load_acq(&fx->ir[r]);
    if (eq.o == IR_EQ && eq.t.irt == (IRT_INT|IRT_GUARD) &&
        (eq.op1 == ctype || eq.op2 == ctype)) { guard = r; break; }
  }
  if (!guard) return 0;
  for (r = call; r > REF_FIRST;) {
    IRRef p = --r;
    if (ir_load_acq(&fx->ir[p]).o == IR_XSAVE) break;
  }
  if (r <= REF_FIRST || ir_load_acq(&fx->ir[r]).o != IR_XSAVE ||
      !snap_for_ref(fx, r, &xsave_snap)) return 0;
  {
    IRIns mark = ir_load_acq(&fx->ir[r]);
    offset = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
  }
  if (offset == 0 || offset >= fx->pt->sizebc) return 0;
  callins = loadbc(&proto_bc(fx->pt)[offset-1u]);
  if (bc_op(callins) != BC_CALL || bc_c(callins) == 0) return 0;
  if (!snap_before(fx, lookup+1u, &pre) || !snap_for_ref(fx, payload, &read) ||
      !snap_after(fx, payload, &after))
    return 0;
  out->read_pc = footer_pc(fx, pre);
  if (!out->read_pc) return 0;
  src = loadbc(out->read_pc);
  if ((bc_op(src) != BC_TGETS && bc_op(src) != BC_TGETV) ||
      snap_ref_acq(&fx->snap[read]) != payload ||
      footer_pc(fx, read) != out->read_pc) return 0;
  out->result_slot = (BCReg)(1u+LJ_FR2+bc_a(src));
  {
    IRRef mapped;
    if (!snapshot_slot(fx, after, out->result_slot, &entry, &mapped) ||
        mapped != payload)
      return 0;
    out->result_entry = entry;
  }
  out->call_pc = &proto_bc(fx->pt)[offset-1u];
  out->call = call;
  out->xsave = r;
  out->lookup = lookup;
  out->root1 = root1;
  out->root2 = root2;
  out->status = lookup+1u;
  out->value = payload;
  out->payload = value;
  out->ctype = ctype;
  out->guard = guard;
  out->pre_snap = pre;
  out->read_snap = read;
  out->after_snap = after;
  out->xsave_snap = xsave_snap;
  out->arg = arg;
  out->call_arg_slot = (BCReg)(bc_a(callins)+2u*(1u+LJ_FR2)+1u+arg);
  if (out->call_arg_slot >= UINT8_MAX ||
      !snapshot_slot(fx, xsave_snap, out->call_arg_slot, &entry, &staged) ||
      staged != payload) return 0;
  out->xsave_entry = entry;
  return 1;
}

/* above helper intentionally stays local: the CARG grammar and source maps
** are read independently of the production admission helpers. */

static void locate_calls(I64Fixture *fx)
{
  IRRef r;
  fx->ncalls = 0;
  for (r = REF_FIRST; r < fx->nins; r++) if (ir_load_acq(&fx->ir[r]).o == IR_CALLXS) {
    IRRef x;
    assert(fx->ncalls < sizeof(fx->calls)/sizeof(fx->calls[0]));
    fx->calls[fx->ncalls] = r;
    fx->xsaves[fx->ncalls] = 0;
    for (x = r; x > REF_FIRST;) {
      IRRef p = --x;
      if (ir_load_acq(&fx->ir[p]).o == IR_XSAVE) {
        fx->xsaves[fx->ncalls] = p; break;
      }
    }
    assert(fx->xsaves[fx->ncalls] != 0);
    fx->ncalls++;
  }
  assert(fx->ncalls >= 2);
  fx->target = fx->ncalls-1u;
  assert(locate_read(fx, fx->calls[fx->target], fx->arg_index, &fx->read));
}

static void reset_fixture(I64Fixture *fx)
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
  locate_calls(fx);
}

static void init_fixture(lua_State *L, jit_State *J, I64Fixture *fx,
                         const char *runner, unsigned arg_index)
{
  memset(fx, 0, sizeof(*fx));
  fx->J = J;
  fx->arg_index = arg_index;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt);
  if (!fx->owner) fprintf(stderr, "table I64 CALLXS trace missing: %s\n", runner);
  assert(fx->owner != NULL);
  fx->cts = ctype_ctsG(G(L));
  fx->nins = semantic_nins(fx->owner);
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

static JSave enter_semantic(I64Fixture *fx)
{
  jit_State *J = fx->J;
  JSave save = { J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace };
  fx->semantic = fx->view_owner;
  fx->semantic.nins = fx->nins;
  fx->ir[fx->ktrace].o = IR_KNUM;
  fx->ir[fx->ktrace].t.irt = IRT_P64;
  fx->ir[fx->ktrace].op12 = 0;
  fx->ir[fx->ktrace].prev = 0;
  fx->ir[fx->ktrace+1u].tv.u64 = 0;
  J->curfinal = NULL;
  J->pt = fx->pt;
  J->startpc = trace_startpc_acq(fx->owner);
  J->loopref = 0;
  for (IRRef r = REF_FIRST; r < fx->nins; r++)
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) { J->loopref = r; break; }
  assert(J->loopref != 0);
  J->parent = 0; J->exitno = 0; J->baseslot = 1u+LJ_FR2;
  J->framedepth = 0; J->retdepth = 0; J->ktrace = fx->ktrace;
  return save;
}

static void leave_semantic(jit_State *J, JSave *s)
{
  J->curfinal=s->curfinal; J->pt=s->pt; J->startpc=s->startpc;
  J->loopref=s->loopref; J->parent=s->parent; J->exitno=s->exitno;
  J->baseslot=s->baseslot; J->framedepth=s->framedepth;
  J->retdepth=s->retdepth; J->ktrace=s->ktrace;
}

static LJArm64PostRAView postra_view(I64Fixture *fx)
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

static int check_semantic(I64Fixture *fx)
{
  uint32_t *pc = (uint32_t *)trace_startpc_acq(fx->owner);
  uint32_t saved = la_load32_acq(pc);
  LJArm64IRReject reject;
  JSave state;
  int ok;
  la_store32_rel(pc, fx->owner->startins);
  state = enter_semantic(fx);
  ok = lj_asm_arm64_ir_admit(fx->J, &fx->semantic, &reject);
  leave_semantic(fx->J, &state);
  la_store32_rel(pc, saved);
  return ok;
}

static int check_postra(I64Fixture *fx)
{
  LJArm64PostRAView v;
  v = postra_view(fx);
  return lj_asm_arm64_postra_admit(&v, NULL);
}

static IRRef other_payload(I64Fixture *fx, IRRef before, IRRef skip)
{
  for (IRRef r = REF_FIRST; r < before; r++) {
    IRIns p = ir_load_acq(&fx->ir[r]);
    if (r != skip && p.o == IR_FLOAD && p.op2 == IRFL_CDATA_INT64 &&
        p.op1 >= REF_FIRST && p.op1 < r &&
        ir_load_acq(&fx->ir[p.op1]).o == IR_VLOAD &&
        irt_type(p.t) == irt_type(ir_load_acq(&fx->ir[skip]).t)) return r;
  }
  return 0;
}

static IRRef other_vload(I64Fixture *fx, IRRef before, IRRef skip)
{
  for (IRRef r = REF_FIRST; r < before; r++)
    if (r != skip && fx->ir[r].o == IR_VLOAD &&
        fx->ir[r].t.irt == (IRT_CDATA|IRT_GUARD)) return r;
  return 0;
}

static void replace_xsave_value(I64Fixture *fx, IRRef value)
{
  I64Read *c = &fx->read;
  fx->snapmap[c->xsave_entry] =
    (fx->snapmap[c->xsave_entry] & ~(SnapEntry)UINT16_MAX) | value;
}

static void mutate(I64Fixture *fx, enum Mutation m)
{
  I64Read *c = &fx->read;
  IRRef callarg[LJ_ARM64_ABI_MAX_ARGS];
  unsigned nargs = carg_values(fx, c->call, callarg, LJ_ARM64_ABI_MAX_ARGS);
  IRRef alt = other_payload(fx, c->call, c->payload);
  IRRef altroot = other_vload(fx, c->call, c->value);
  assert(nargs >= 9 && alt != 0 && altroot != 0);
  switch (m) {
  case M_HELPER:
    fx->ir[c->lookup].op2 = IRCALL_lj_tab_getscalar_rooted_try; break;
  case M_STATUS: {
    IRRef k = fx->ir[c->status].op1 == c->lookup ?
      fx->ir[c->status].op2 : fx->ir[c->status].op1;
    assert(k >= trace_nk_acq(fx->owner) && fx->ir[k].o == IR_KINT);
    fx->ir[k].i = 1; break;
  }
  case M_ROOT1_MODE: fx->ir[c->root1].op2 = IRTMPREF_IN1; break;
  case M_ROOT2_MODE: fx->ir[c->root2].op2 = IRTMPREF_IN1; break;
  case M_LOOKUP_CARG: {
    IRRef carg = fx->ir[c->lookup].op1;
    assert(fx->ir[carg].o == IR_CARG);
    fx->ir[carg].op2 = c->root2;
    break;
  }
  case M_RECEIVER_GUARD: {
    IRRef table = fx->ir[c->root1].op1;
    assert(table >= REF_FIRST && fx->ir[table].o == IR_SLOAD &&
           irt_type(fx->ir[table].t) == IRT_TAB &&
           (fx->ir[table].t.irt & IRT_GUARD) != 0 &&
           fx->ir[table].op2 == IRSLOAD_TYPECHECK);
    fx->ir[table].op2 = IRSLOAD_READONLY;
    break;
  }
  case M_VLOAD_BASE: fx->ir[c->value].op1 = c->root2; break;
  case M_VLOAD_OFFSET: fx->ir[c->value].op2 = 1; break;
  case M_VLOAD_TYPE: fx->ir[c->value].t.irt = IRT_TAB|IRT_GUARD; break;
  case M_VLOAD_GUARD: fx->ir[c->value].t.irt = IRT_CDATA; break;
  case M_CTYPE_SOURCE: fx->ir[c->ctype].op1 = altroot; break;
  case M_CTYPE_WIDTH: fx->ir[c->ctype].t.irt = IRT_U32; break;
  case M_CTYPE_GUARD: fx->ir[c->guard].o = IR_NE; break;
  case M_PAYLOAD_SOURCE: fx->ir[c->payload].op1 = altroot; break;
  case M_PAYLOAD_WIDTH: fx->ir[c->payload].t.irt = IRT_INT; break;
  case M_PAYLOAD_SIGN: fx->ir[c->payload].t.irt =
      irt_type(fx->ir[c->payload].t) == IRT_I64 ? IRT_U64 : IRT_I64; break;
  case M_CARG_OTHER: set_carg(fx, c->call, c->arg, alt); break;
  case M_READ_PC: set_footer_pc(fx, c->pre_snap, c->read_pc+1u); break;
  case M_POSTREAD_PC: set_footer_pc(fx, c->read_snap, c->read_pc+1u); break;
  case M_RESULT_SLOT: {
    SnapEntry *se = &fx->snapmap[c->result_entry];
    *se = (*se & ~(SnapEntry)UINT16_MAX) | altroot;
    break;
  }
  case M_XSAVE_OTHER: replace_xsave_value(fx, altroot); break;
  case M_XSAVE_NORESTORE: fx->snapmap[c->xsave_entry] |= SNAP_NORESTORE; break;
  case M_XSAVE_TRUNCATED:
    assert(fx->snap[c->xsave_snap].nslots > c->call_arg_slot);
    fx->snap[c->xsave_snap].nslots = c->call_arg_slot; break;
  default: assert(!"post-RA-only mutation in semantic path");
  }
}

static void expect_both(I64Fixture *fx, enum Mutation m, const char *name)
{
  int accepted;
  reset_fixture(fx); mutate(fx, m); accepted = check_semantic(fx);
  if (accepted) fprintf(stderr, "table I64 semantic accepted %s\n", name);
  assert(!accepted);
  reset_fixture(fx); mutate(fx, m); accepted = check_postra(fx);
  if (accepted) fprintf(stderr, "table I64 post-RA accepted %s\n", name);
  assert(!accepted);
}

static void expect_postra(I64Fixture *fx, enum Mutation m, const char *name)
{
  I64Read *c;
  int accepted;
  reset_fixture(fx); c = &fx->read;
  if (m == M_ROOT_ALIAS) fx->ir[c->root2].r = fx->ir[c->root1].r;
  else if (m == M_ROOT_SPILL) fx->ir[c->root1].s = SPS_FIRST;
  else if (m == M_VLOAD_SPILL_HALF)
    fx->ir[c->value].s = (uint8_t)(SPS_FIRST+1u);
  else if (m == M_VLOAD_SPILL_OUTSIDE) {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(cap < SPS_LIMIT); fx->ir[c->value].s = (uint8_t)cap;
  }
  else if (m == M_PAYLOAD_SPILL_HALF) fx->ir[c->payload].s = SPS_FIRST+1u;
  else if (m == M_PAYLOAD_SPILL_OUTSIDE) {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(cap < SPS_LIMIT); fx->ir[c->payload].s = (uint8_t)cap;
  } else assert(!"unexpected post-RA mutation");
  accepted = check_postra(fx);
  if (accepted) fprintf(stderr, "table I64 post-RA accepted %s\n", name);
  assert(!accepted);
}

static void run_lua(lua_State *L, const char *src)
{
  int status = luaL_dostring(L, src);
  if (status != LUA_OK) {
    fprintf(stderr, "table I64 admission setup: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void run_case(lua_State *L, const char *runner, const char *fn,
                     const char *counter)
{
  lua_getglobal(L, "__table_i64_configure");
  lua_pushinteger(L, 0);
  assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
  lua_getglobal(L, "__table_i64_driver");
  lua_getglobal(L, runner); lua_getglobal(L, fn);
  lua_getglobal(L, "__table_i64_registry");
  lua_pushstring(L, "a0"); lua_pushinteger(L, 32);
  if (lua_pcall(L, 5, 1, 0) != LUA_OK) {
    fprintf(stderr, "table I64 runner failed: %s\n", lua_tostring(L, -1));
    assert(0);
  }
  assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 32);
  lua_pop(L, 1);
  lua_getglobal(L, counter);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1) &&
         lua_tointeger(L, -1) == 32);
  lua_pop(L, 1);
}

static void check_original(I64Fixture *fx)
{
  uint32_t *pc = (uint32_t *)trace_startpc_acq(fx->owner);
  uint32_t saved = la_load32_acq(pc);
  JSave state;
  LJArm64PostRAView v;
  reset_fixture(fx);
  la_store32_rel(pc, fx->owner->startins);
  state = enter_semantic(fx);
  assert(lj_asm_arm64_ir_admit(fx->J, &fx->semantic, NULL));
  leave_semantic(fx->J, &state);
  la_store32_rel(pc, saved);
  reset_fixture(fx); v = postra_view(fx);
  assert(lj_asm_arm64_postra_admit(&v, NULL));
}

static void verify_fixed_signature(lua_State *L, const char *name,
                                   unsigned is_unsigned)
{
  LJArm64ABISignature abi;
  unsigned i;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  assert(lj_ccall_arm64_signature(ctype_ctsG(G(L)),
         cdataV(L->top-1)->ctypeid, &abi));
  lua_pop(L, 1);
  assert(abi.nargs == 9 && abi.ngpr == 8 && abi.stack_size == 8);
  for (i = 0; i < 9; i++)
    assert(abi.arg[i].size == 8 && abi.arg[i].cls == LJ_ARM64_ABI_GPR &&
           abi.arg[i].is_unsigned == is_unsigned &&
           abi.arg[i].reg == (i < 8 ? i : LJ_ARM64_ABI_STACK));
  assert(abi.arg[8].stack_offset == 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  I64Fixture signedfx, unsignedfx, tvfx;
  const char *lib = getenv("LJ_M7_FFI_TABLE_I64_ARGS_SO");
  assert(lib && *lib);
  run_lua(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_callxs_table_i64_configure(int32_t); "
    "int32_t lj_callxs_table_i64_signed_count(void); "
    "int32_t lj_callxs_table_i64_unsigned_count(void); "
    "int32_t lj_callxs_table_i64_signed(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t); "
    "int32_t lj_callxs_table_i64_unsigned(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_TABLE_I64_ARGS_SO')))\n"
    "local S={ffi.new('int64_t',0x8000000000000000LL),ffi.new('int64_t',0x7fffffffffffffffLL),ffi.new('int64_t',-1),ffi.new('int64_t',0x0123456789abcdefLL),ffi.new('int64_t',-81985529216486896LL),ffi.new('int64_t',-9223372036854775807LL),ffi.new('int64_t',0x7fffffff),ffi.new('int64_t',0x80000000),ffi.new('int64_t',0x100000000)}\n"
    "local U={ffi.new('uint64_t',0ULL),ffi.new('uint64_t',0xffffffffffffffffULL),ffi.new('uint64_t',0x8000000000000000ULL),ffi.new('uint64_t',0x0123456789abcdefULL),ffi.new('uint64_t',0xfedcba9876543210ULL),ffi.new('uint64_t',0x100000001ULL),ffi.new('uint64_t',0x7fffffffULL),ffi.new('uint64_t',0x80000000ULL),ffi.new('uint64_t',0xffffffffULL)}\n"
    "local function named(src) local t={}; for i=1,9 do t['a'..(i-1)]=src[i] end; return t end\n"
    "_G.__table_i64_registry={s=named(S),u=named(U)}\n"
    "_G.__table_i64_signed=lib.lj_callxs_table_i64_signed; _G.__table_i64_unsigned=lib.lj_callxs_table_i64_unsigned\n"
    "_G.__table_i64_signed_count=lib.lj_callxs_table_i64_signed_count; _G.__table_i64_unsigned_count=lib.lj_callxs_table_i64_unsigned_count\n"
    "_G.__table_i64_configure=function(m) lib.lj_callxs_table_i64_configure(m) end\n"
    "function _G.__table_i64_run_s(fn,reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do sum=sum+fn(s.a0,s.a1,s.a2,s.a3,s.a4,s.a5,s.a6,s.a7,s.a8) end; return sum-0.5 end\n"
    "function _G.__table_i64_run_v(fn,reg,key,n) local s=reg.s; local sum=0.5; for i=1,n do local a=s[key]; sum=sum+fn(a,s.a1,s.a2,s.a3,s.a4,s.a5,s.a6,s.a7,s.a8) end; return sum-0.5 end\n"
    "function _G.__table_u64_run_v(fn,reg,key,n) local u=reg.u; local sum=0.5; for i=1,n do local a=u[key]; sum=sum+fn(a,u.a1,u.a2,u.a3,u.a4,u.a5,u.a6,u.a7,u.a8) end; return sum-0.5 end\n"
    "_G.__ready=threading.channel(1); _G.__stop=threading.channel(1); _G.__peer=threading.spawn(function(r,s) assert(r:send(true,5)); local _,ok=s:recv(30); assert(ok); return true end,__ready,__stop); local _,ok=__ready:recv(5); assert(ok)\n"
    "function _G.__table_i64_driver(run,fn,reg,key,n) local result=run(fn,reg,key,n); return result end\n"
    "lib.lj_callxs_table_i64_configure(0); jit.on(); jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");
  verify_fixed_signature(L, "__table_i64_signed", 0);
  verify_fixed_signature(L, "__table_i64_unsigned", 1);
  run_case(L, "__table_i64_run_s", "__table_i64_signed",
           "__table_i64_signed_count");
  init_fixture(L, J, &signedfx, "__table_i64_run_s", 0);
  check_original(&signedfx);
  expect_both(&signedfx, M_HELPER, "wrong rooted hit helper");
  expect_both(&signedfx, M_STATUS, "wrong helper status value");
  expect_both(&signedfx, M_ROOT1_MODE, "result scratch mode");
  expect_both(&signedfx, M_ROOT2_MODE, "key scratch mode");
  expect_both(&signedfx, M_LOOKUP_CARG, "lookup output CARG uses key root");
  expect_both(&signedfx, M_RECEIVER_GUARD,
              "table receiver SLOAD lacks type-check mode");
  expect_both(&signedfx, M_VLOAD_BASE, "CData VLOAD from wrong root");
  expect_both(&signedfx, M_VLOAD_OFFSET, "CData VLOAD nonzero offset");
  expect_both(&signedfx, M_VLOAD_TYPE, "CData VLOAD wrong type");
  expect_both(&signedfx, M_VLOAD_GUARD, "CData VLOAD missing guard");
  expect_both(&signedfx, M_CTYPE_SOURCE, "CData type load from another value");
  expect_both(&signedfx, M_CTYPE_WIDTH, "CData type load wrong width");
  expect_both(&signedfx, M_CTYPE_GUARD, "CData type equality not guarded");
  expect_both(&signedfx, M_PAYLOAD_SOURCE, "payload from another root");
  expect_both(&signedfx, M_PAYLOAD_WIDTH, "payload wrong width");
  expect_both(&signedfx, M_PAYLOAD_SIGN, "payload signedness mismatch");
  expect_both(&signedfx, M_CARG_OTHER, "CARG uses another table value");
  expect_both(&signedfx, M_READ_PC, "lookup snapshot skips table read");
  expect_both(&signedfx, M_POSTREAD_PC, "VLOAD snapshot skips table read");
  expect_both(&signedfx, M_RESULT_SLOT,
              "later snapshot loses fetched table value");
  expect_both(&signedfx, M_XSAVE_OTHER, "XSAVE stages another CData");
  expect_both(&signedfx, M_XSAVE_NORESTORE, "XSAVE CData not restored");
  expect_both(&signedfx, M_XSAVE_TRUNCATED, "XSAVE argument span truncated");
  expect_postra(&signedfx, M_ROOT_ALIAS, "table/key root register alias");
  expect_postra(&signedfx, M_ROOT_SPILL, "table scratch spilled");
  expect_postra(&signedfx, M_VLOAD_SPILL_HALF,
                "CData root VLOAD half-slot spill");
  expect_postra(&signedfx, M_VLOAD_SPILL_OUTSIDE,
                "CData root VLOAD outside frame spill");
  expect_postra(&signedfx, M_PAYLOAD_SPILL_HALF, "I64 payload half-slot spill");
  expect_postra(&signedfx, M_PAYLOAD_SPILL_OUTSIDE, "I64 payload spill outside frame");

  run_case(L, "__table_u64_run_v", "__table_i64_unsigned",
           "__table_i64_unsigned_count");
  init_fixture(L, J, &unsignedfx, "__table_u64_run_v", 8);
  check_original(&unsignedfx);
  expect_both(&unsignedfx, M_PAYLOAD_SIGN, "U64 payload signedness mismatch");
  expect_both(&unsignedfx, M_PAYLOAD_WIDTH, "stack U64 payload wrong width");

  run_case(L, "__table_i64_run_v", "__table_i64_signed",
           "__table_i64_signed_count");
  init_fixture(L, J, &tvfx, "__table_i64_run_v", 0);
  check_original(&tvfx);
  expect_both(&tvfx, M_READ_PC, "TGETV source PC not retained");

  free(signedfx.ir); free(signedfx.snap); free(signedfx.snapmap);
  free(unsignedfx.ir); free(unsignedfx.snap); free(unsignedfx.snapmap);
  free(tvfx.ir); free(tvfx.snap); free(tvfx.snapmap);
  run_lua(L, "assert(__stop:send(true,5)); local ok,result=__peer:join(5); assert(ok and result)");
  lua_close(L);
  puts("t-arm64-jit-table-i64-args-admission OK: independent table-rooted I64 proofs");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-i64-args-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}

#endif
