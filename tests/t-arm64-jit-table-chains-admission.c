/* Independent admission tests for CALLXS using a CData value read from a
** shared Lua table. */

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
    defined(LJ_TRACE_TEST_HELPERS)

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
#error "table CALLXS admission requires ARM64 CALLXS admission"
#endif

enum { TABLE_CALL_MAX = 16 };

typedef struct TableCall {
  IRRef lookup, root1, root2, status, value;
  IRRef receiver, receiver_lookup, receiver_root1, receiver_root2;
  IRRef receiver_parent;
  IRRef receiver_status;
  IRRef call, xsave, pointer, ctype;
  MSize pre_snap, post_snap, after_snap, snap;
  MSize receiver_pre_snap, receiver_read_snap, receiver_after_snap;
  BCReg read_slot, cdata_slot;
  BCReg receiver_slot;
  BCReg receiver_input_slot;
  MSize cdata_entry, receiver_entry, receiver_input_entry;
  MSize receiver_after_entry;
  const BCIns *read_pc, *call_pc, *receiver_pc;
} TableCall;

typedef struct TableFixture {
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
  TableCall calls[TABLE_CALL_MAX];
  unsigned ncalls, target, other, livecall;
} TableFixture;

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
  M_WRONG_HELPER,
  M_WRONG_STATUS,
  M_ROOT1_MODE,
  M_ROOT2_MODE,
  M_RECEIVER_GUARD,
  M_CARG_OUTPUT,
  M_VLOAD_BASE,
  M_VLOAD_OFFSET,
  M_VLOAD_TYPE,
  M_VLOAD_GUARD,
  M_CHAIN_RECEIVER_SOURCE,
  M_CHAIN_RECEIVER_SLOT,
  M_CHAIN_RECEIVER_NORESTORE,
  M_XSAVE_TABLE_OTHER,
  M_XSAVE_TABLE_NORESTORE,
  M_LIVE_ROUTE_XSAVE_TABLE_OTHER,
  M_LIVE_ROUTE_XSAVE_TABLE_NORESTORE,
  M_LIVE_ROUTE_XSAVE_TABLE_TRUNCATED,
  M_POSTREAD_SNAPSHOT,
  M_STATUS_PC,
  M_POSTREAD_PC,
  M_RESULT_PC,
  M_POINTER_OTHER_SOURCE,
  M_CTYPE_OTHER_SOURCE,
  M_XSAVE_OTHER_CDATA,
  M_XSAVE_GC_SLOT_ALIAS,
  M_XSAVE_NORESTORE,
  M_XSAVE_TRUNCATED,
  M_ROOT_ALIAS,
  M_ROOT_SPILL,
  M_VLOAD_SPILL_HALF,
  M_VLOAD_SPILL_OUTSIDE,
  M_RECEIVER_SPILL_HALF,
  M_RECEIVER_SPILL_OUTSIDE
};

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
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

static CTypeID global_ctype(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return id;
}

static int global_cdata_ctype(lua_State *L, const char *name, CTypeID *id)
{
  int found;
  lua_getglobal(L, name);
  found = tviscdata(L->top-1);
  if (found) *id = cdataV(L->top-1)->ctypeid;
  lua_pop(L, 1);
  return found;
}

static unsigned op_count(const GCtrace *T, IROp op)
{
  IRRef ref;
  unsigned count = 0;
  const IRIns *ir = trace_ir_acq(T);
  for (ref = REF_FIRST; ref < trace_nins_acq((GCtrace *)T); ref++)
    if (ir_load_acq(&ir[ref]).o == op) count++;
  return count;
}

static unsigned hit_count(const GCtrace *T)
{
  IRRef ref;
  unsigned count = 0;
  const IRIns *ir = trace_ir_acq(T);
  for (ref = REF_FIRST; ref < trace_nins_acq((GCtrace *)T); ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_CALLS &&
        ins.op2 == IRCALL_lj_tab_gettv_rooted_hit_try) count++;
  }
  return count;
}

static IRRef find_ktrace(const GCtrace *T)
{
  IRRef ref, nk = trace_nk_acq((GCtrace *)T);
  const IRIns *ir = trace_ir_acq(T);
  for (ref = nk; ref < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[ref]) == obj2gco(T))
      return ref;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      ref += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      ref++;
    else
      return 0;
  }
  return 0;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq((GCtrace *)T);
  IRRef nins = trace_nins_acq((GCtrace *)T);
  while (nins > REF_BASE) {
    IRIns tail = ir_load_acq(&ir[nins-1u]);
    if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL &&
        tail.op1 >= REF_FIRST && tail.op1 < nins-1u &&
        tail.op2 < trace_nsnap_acq((GCtrace *)T) && tail.s == SPS_NONE) {
      nins--;
      continue;
    }
    if (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
        tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0) {
      nins--;
      continue;
    }
    break;
  }
  return nins;
}

static GCtrace *find_trace(jit_State *J, GCproto *pt)
{
  TraceNo no;
  for (no = 1; (MSize)no < trace_sizetrace_acq(J); no++) {
    GCtrace *T = traceref_safe(J, no);
    unsigned calls, reads;
    if (T == NULL || !trace_runnable_acq(T, no) ||
        trace_root_acq(T) != 0 || trace_startpt_acq(T) != pt ||
        bc_op(trace_startins_acq(T)) != BC_FORL)
      continue;
    calls = op_count(T, IR_CALLXS);
    reads = hit_count(T);
    if (calls >= 2u && reads >= 2u)
      return T;
  }
  return NULL;
}

static int snapshot_slot(TableFixture *fx, MSize snapno, BCReg slot,
                         MSize *entryp, IRRef *refp)
{
  MSize ofs, nent, i;
  unsigned found = 0;
  const SnapShot *snap;
  if (snapno >= fx->nsnap) return 0;
  snap = &fx->snap[snapno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
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

static int snapshot_for_ref(TableFixture *fx, IRRef ref, MSize *snapno)
{
  MSize s;
  for (s = 0; s < fx->nsnap; s++)
    if (snap_ref_acq(&fx->snap[s]) == ref) {
      *snapno = s;
      return 1;
    }
  return 0;
}

static int snapshot_at_or_before(TableFixture *fx, IRRef ref, MSize *snapno)
{
  MSize s, found = UINT_MAX;
  IRRef best = 0;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at <= ref && at >= best) {
      best = at;
      found = s;
    }
  }
  if (found == UINT_MAX) return 0;
  *snapno = found;
  return 1;
}

static int snapshot_first_after(TableFixture *fx, IRRef ref, MSize *snapno)
{
  MSize s, found = UINT_MAX;
  IRRef best = REF_DROP;
  for (s = 0; s < fx->nsnap; s++) {
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at > ref && at < best) {
      best = at;
      found = s;
    }
  }
  if (found == UINT_MAX) return 0;
  *snapno = found;
  return 1;
}

static const BCIns *snapshot_footer_pc(TableFixture *fx, MSize snapno)
{
  MSize ofs, nent;
  if (snapno >= fx->nsnap) return NULL;
  ofs = snap_mapofs_acq(&fx->snap[snapno]);
  nent = snap_nent_acq(&fx->snap[snapno]);
  if (ofs > fx->nsnapmap || nent > fx->nsnapmap-ofs ||
      1u+LJ_FR2 > fx->nsnapmap-ofs-nent)
    return NULL;
  return snap_pc(&fx->snapmap[ofs+nent]);
}

static int snapshot_maps_equal(TableFixture *fx, MSize a, MSize b)
{
  MSize oa, ob, na, nb, i;
  if (a >= fx->nsnap || b >= fx->nsnap ||
      fx->snap[a].nslots != fx->snap[b].nslots ||
      fx->snap[a].topslot != fx->snap[b].topslot)
    return 0;
  oa = snap_mapofs_acq(&fx->snap[a]);
  ob = snap_mapofs_acq(&fx->snap[b]);
  na = snap_nent_acq(&fx->snap[a]);
  nb = snap_nent_acq(&fx->snap[b]);
  if (na != nb || oa > fx->nsnapmap || na > fx->nsnapmap-oa ||
      ob > fx->nsnapmap || nb > fx->nsnapmap-ob)
    return 0;
  for (i = 0; i < na; i++)
    if (fx->snapmap[oa+i] != fx->snapmap[ob+i]) return 0;
  return 1;
}

static void snapshot_set_footer_pc(TableFixture *fx, MSize snapno,
                                   const BCIns *pc)
{
  MSize ofs, nent;
  SnapEntry *footer;
  assert(pc != NULL && snapno < fx->nsnap);
  ofs = snap_mapofs_acq(&fx->snap[snapno]);
  nent = snap_nent_acq(&fx->snap[snapno]);
  assert(ofs <= fx->nsnapmap && nent <= fx->nsnapmap-ofs &&
         1u+LJ_FR2 <= fx->nsnapmap-ofs-nent);
  footer = &fx->snapmap[ofs+nent];
#if LJ_FR2
  {
    uint64_t packed;
    memcpy(&packed, footer, sizeof(packed));
    packed = ((uint64_t)(uintptr_t)pc << 8) | (packed & 0xffu);
    memcpy(footer, &packed, sizeof(packed));
  }
#else
  footer[0] = SNAP_MKPC(pc);
#endif
}

static int safe_call_setup(BCOp op)
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
    if (!safe_call_setup(bc_op(loadbc(pc)))) return 0;
  return 1;
}

static const BCIns *call_after_read_setup(const GCproto *pt,
                                         const BCIns *readpc)
{
  const BCIns *pc = readpc+1u;
  const BCIns *end = proto_bc(pt)+pt->sizebc;
  while (pc < end) {
    BCOp op = bc_op(loadbc(pc));
    if (op == BC_CALL) return pc;
    if (!safe_call_setup(op)) return NULL;
    pc++;
  }
  return NULL;
}

static int locate_receiver_read(TableFixture *fx, TableCall *c)
{
  IRIns value, call, tail, head, status, input, r1, r2;
  IRRef readref, keyref, statusvalue;
  MSize pre, at, after, entry;
  BCIns source;
  const BCIns *pc, *nextpc;
  BCReg table_slot, key_slot, dst_slot;
  IRRef mapped;
  value = ir_load_acq(&fx->ir[c->receiver]);
  if (value.o != IR_VLOAD || value.t.irt != (IRT_TAB|IRT_GUARD) ||
      value.op2 || c->receiver < REF_FIRST+2u)
    return 0;
  readref = c->receiver-2u;
  call = ir_load_acq(&fx->ir[readref]);
  if (call.o != IR_CALLS ||
      call.op2 != IRCALL_lj_tab_gettv_rooted_hit_try ||
      call.op1 < REF_FIRST || call.op1 >= readref)
    return 0;
  status = ir_load_acq(&fx->ir[readref+1u]);
  if (status.o != IR_NE || status.t.irt != (IRT_INT|IRT_GUARD) ||
      !((status.op1 == readref && irref_isk(status.op2)) ||
        (status.op2 == readref && irref_isk(status.op1))))
    return 0;
  statusvalue = status.op1 == readref ? status.op2 : status.op1;
  if (statusvalue < trace_nk_acq(fx->owner) ||
      fx->ir[statusvalue].o != IR_KINT || fx->ir[statusvalue].i != 0)
    return 0;
  tail = ir_load_acq(&fx->ir[call.op1]);
  if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
      tail.op1 < REF_FIRST || tail.op1 >= call.op1 ||
      tail.op2 < REF_FIRST || tail.op2 >= readref)
    return 0;
  head = ir_load_acq(&fx->ir[tail.op1]);
  if (head.o != IR_CARG || head.t.irt != IRT_NIL ||
      head.op1 != tail.op2 || head.op2 < REF_FIRST || head.op2 >= readref)
    return 0;
  r1 = ir_load_acq(&fx->ir[tail.op2]);
  r2 = ir_load_acq(&fx->ir[head.op2]);
  if (r1.o != IR_TMPREF || r1.t.irt != IRT_PGC ||
      r1.op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
      r2.o != IR_TMPREF || r2.t.irt != IRT_PGC ||
      r2.op2 != IRTMPREF_IN2)
    return 0;
  c->receiver_lookup = readref;
  c->receiver_status = readref+1u;
  c->receiver_root1 = tail.op2;
  c->receiver_root2 = head.op2;
  c->receiver_parent = ir_load_acq(&fx->ir[tail.op2]).op1;
  c->receiver_input_slot = UINT8_MAX;
  if (c->receiver_parent < REF_FIRST || c->receiver_parent >= readref)
    return 0;
  input = ir_load_acq(&fx->ir[c->receiver_parent]);
  if (input.o == IR_SLOAD) {
    if (irt_type(input.t) != IRT_TAB || !irt_isguard(input.t) ||
        input.op2 != IRSLOAD_TYPECHECK)
      return 0;
  } else if (input.o == IR_VLOAD) {
    if (input.t.irt != (IRT_TAB|IRT_GUARD) ||
        c->receiver_parent >= readref)
      return 0;
  } else {
    return 0;
  }
  keyref = ir_load_acq(&fx->ir[c->receiver_root2]).op1;
  if (keyref < trace_nk_acq(fx->owner) || keyref >= readref) return 0;
  if (!snapshot_at_or_before(fx, readref+1u, &pre) ||
      !snapshot_for_ref(fx, c->receiver, &at) ||
      !snapshot_first_after(fx, c->receiver, &after))
    return 0;
  pc = snapshot_footer_pc(fx, pre);
  if (!pc || snapshot_footer_pc(fx, at) != pc ||
      !snapshot_maps_equal(fx, pre, at) ||
      snap_ref_acq(&fx->snap[pre]) > readref-1u ||
      snap_ref_acq(&fx->snap[at]) != c->receiver)
    return 0;
  source = loadbc(pc);
  if (bc_op(source) != BC_TGETS && bc_op(source) != BC_TGETV &&
      bc_op(source) != BC_TGETB)
    return 0;
  table_slot = (BCReg)(1u+LJ_FR2+bc_b(source));
  dst_slot = (BCReg)(1u+LJ_FR2+bc_a(source));
  if (bc_b(source) >= fx->pt->framesize || bc_a(source) >= fx->pt->framesize ||
      !snapshot_slot(fx, pre, table_slot, &entry, &mapped) ||
      mapped != c->receiver_parent ||
      !snapshot_slot(fx, at, table_slot, NULL, &mapped) ||
      mapped != c->receiver_parent)
    return 0;
  c->receiver_input_slot = table_slot;
  c->receiver_input_entry = entry;
  if (bc_op(source) == BC_TGETV) {
    key_slot = (BCReg)(1u+LJ_FR2+bc_c(source));
    if (bc_c(source) >= fx->pt->framesize ||
        !snapshot_slot(fx, pre, key_slot, NULL, &mapped) || mapped != keyref ||
        !snapshot_slot(fx, at, key_slot, NULL, &mapped) || mapped != keyref)
      return 0;
  } else if (bc_op(source) == BC_TGETS) {
    if (ir_load_acq(&fx->ir[keyref]).o != IR_KGC ||
        ir_load_acq(&fx->ir[keyref]).t.irt != IRT_STR ||
        bc_c(source) >= fx->pt->sizekgc ||
        ir_kgc_load_acq(&fx->ir[keyref]) !=
          proto_kgc_acq(fx->pt, ~(int32_t)bc_c(source)))
      return 0;
  } else if (ir_load_acq(&fx->ir[keyref]).o != IR_KINT ||
             ir_load_acq(&fx->ir[keyref]).i != (int32_t)bc_c(source)) {
    return 0;
  }
  nextpc = snapshot_footer_pc(fx, after);
  if (!nextpc || nextpc <= pc || !safe_setup_range(pc, nextpc) ||
      !snapshot_slot(fx, after, dst_slot, &entry, &mapped) ||
      mapped != c->receiver)
    return 0;
  c->receiver_pre_snap = pre;
  c->receiver_read_snap = at;
  c->receiver_after_snap = after;
  c->receiver_slot = dst_slot;
  c->receiver_after_entry = entry;
  c->receiver_pc = pc;
  return 1;
}

static int locate_call(TableFixture *fx, IRRef callref, TableCall *c)
{
  IRIns call, pointer;
  IRRef fpref, cdref, r, xsave = 0, lookup = 0;
  IRRef root1 = 0, root2 = 0, status = 0, ctype = 0;
  MSize snapno, presnap, postsnap, aftersnap, slot, readslot, entry;
  IRRef source_ref;
  uint32_t offset;
  BCIns source;
  const BCIns *afterpc;
  BCReg tableslot;
  memset(c, 0, sizeof(*c));
  call = ir_load_acq(&fx->ir[callref]);
  if (call.o != IR_CALLXS || call.op2 < REF_FIRST || call.op2 >= callref)
    return 0;
  fpref = call.op2;
  pointer = ir_load_acq(&fx->ir[fpref]);
  if (pointer.o != IR_FLOAD || pointer.op2 != IRFL_CDATA_PTR ||
      pointer.op1 < REF_FIRST || pointer.op1 >= fpref)
    return 0;
  cdref = pointer.op1;
  if (ir_load_acq(&fx->ir[cdref]).o != IR_VLOAD ||
      ir_load_acq(&fx->ir[cdref]).t.irt != (IRT_CDATA|IRT_GUARD))
    return 0;
  for (r = REF_FIRST; r < callref; r++) {
    IRIns ins = ir_load_acq(&fx->ir[r]);
    if (ins.o == IR_CALLS &&
        ins.op2 == IRCALL_lj_tab_gettv_rooted_hit_try && r+2u == cdref) {
      IRIns tail, head;
      lookup = r;
      status = r+1u;
      tail = ir_load_acq(&fx->ir[ins.op1]);
      if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
          tail.op1 < REF_FIRST || tail.op1 >= ins.op1)
        return 0;
      root1 = tail.op2;
      head = ir_load_acq(&fx->ir[tail.op1]);
      if (head.o != IR_CARG || head.t.irt != IRT_NIL)
        return 0;
      root2 = head.op2;
      break;
    }
  }
  if (!lookup || !root1 || !root2) return 0;
  for (r = REF_FIRST; r < callref; r++) {
    IRIns ins = ir_load_acq(&fx->ir[r]);
    if (ins.o == IR_FLOAD && ins.op1 == cdref &&
        ins.op2 == IRFL_CDATA_CTYPEID) {
      ctype = r;
      break;
    }
  }
  for (r = callref; r > REF_FIRST;) {
    IRRef prev = --r;
    if (ir_load_acq(&fx->ir[prev]).o == IR_XSAVE) {
      xsave = prev;
      break;
    }
  }
  if (!ctype || !xsave || !snapshot_for_ref(fx, xsave, &snapno)) return 0;
  {
    IRIns marker = ir_load_acq(&fx->ir[xsave]);
    if (marker.o != IR_XSAVE) return 0;
    offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
  }
  if (offset == 0 || offset >= fx->pt->sizebc) return 0;
  source = loadbc(&proto_bc(fx->pt)[offset-1u]);
  if (bc_op(source) != BC_CALL) return 0;
  if (offset < 2u) return 0;
  c->call_pc = &proto_bc(fx->pt)[offset-1u];
  if (!snapshot_at_or_before(fx, status, &presnap)) return 0;
  c->read_pc = snapshot_footer_pc(fx, presnap);
  if (!c->read_pc) return 0;
  if ((bc_op(loadbc(c->read_pc)) != BC_TGETS &&
       bc_op(loadbc(c->read_pc)) != BC_TGETV) ||
      bc_a(loadbc(c->read_pc)) != bc_a(source) ||
      call_after_read_setup(fx->pt, c->read_pc) != c->call_pc) return 0;
  if (!snapshot_at_or_before(fx, status, &presnap) ||
      !snapshot_for_ref(fx, cdref, &postsnap) ||
      !snapshot_first_after(fx, cdref, &aftersnap) ||
      snap_ref_acq(&fx->snap[presnap]) >= cdref ||
      snapshot_footer_pc(fx, presnap) != c->read_pc ||
      snapshot_footer_pc(fx, postsnap) != c->read_pc ||
      !snapshot_maps_equal(fx, presnap, postsnap) ||
      snap_ref_acq(&fx->snap[aftersnap]) <= cdref) return 0;
  afterpc = snapshot_footer_pc(fx, aftersnap);
  if (!afterpc || afterpc <= c->read_pc || afterpc > c->call_pc ||
      !safe_setup_range(c->read_pc, afterpc)) return 0;
  readslot = 1u+LJ_FR2+(MSize)bc_a(loadbc(c->read_pc));
  if (readslot >= UINT8_MAX ||
      !snapshot_slot(fx, aftersnap, (BCReg)readslot, NULL, &source_ref) ||
      source_ref != cdref) return 0;
  tableslot = (BCReg)(1u+LJ_FR2+bc_b(loadbc(c->read_pc)));
  if (bc_b(loadbc(c->read_pc)) >= fx->pt->framesize ||
      !snapshot_slot(fx, presnap, tableslot, NULL, &source_ref) ||
      source_ref != ir_load_acq(&fx->ir[root1]).op1 ||
      !snapshot_slot(fx, postsnap, tableslot, NULL, &source_ref) ||
      source_ref != ir_load_acq(&fx->ir[root1]).op1) return 0;
  slot = 1u+LJ_FR2+(MSize)bc_a(source)+1u+LJ_FR2;
  if (slot >= UINT8_MAX ||
      !snapshot_slot(fx, snapno, (BCReg)slot, &entry, &source_ref) ||
      source_ref != cdref) return 0;
  c->lookup = lookup;
  c->root1 = root1;
  c->root2 = root2;
  c->status = status;
  c->value = cdref;
  c->pre_snap = presnap;
  c->post_snap = postsnap;
  c->after_snap = aftersnap;
  c->read_slot = (BCReg)readslot;
  c->call = callref;
  c->xsave = xsave;
  c->pointer = fpref;
  c->ctype = ctype;
  c->snap = snapno;
  c->cdata_slot = (BCReg)slot;
  c->cdata_entry = entry;
  c->receiver = ir_load_acq(&fx->ir[root1]).op1;
  if (!locate_receiver_read(fx, c)) return 0;
  if (snapshot_slot(fx, snapno, c->receiver_slot, &c->receiver_entry,
                    &source_ref) && source_ref == c->receiver) {
    /* This receiver remains live across the native call. */
  } else {
    c->receiver_entry = (MSize)-1;
  }
  return 1;
}

static void locate_calls(TableFixture *fx)
{
  IRRef ref;
  fx->ncalls = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(fx->owner); ref++) {
    if (ir_load_acq(&fx->ir[ref]).o == IR_CALLXS) {
      TableCall c;
      assert(fx->ncalls < TABLE_CALL_MAX);
      assert(locate_call(fx, ref, &c));
      fx->calls[fx->ncalls++] = c;
    }
  }
  assert(fx->ncalls >= 2);
  fx->target = fx->ncalls;
  fx->other = fx->ncalls;
  for (unsigned i = 0; i < fx->ncalls && fx->target == fx->ncalls; i++)
    for (unsigned j = 0; j < fx->ncalls; j++)
      if (fx->calls[i].receiver_parent == fx->calls[j].receiver &&
          fx->calls[i].receiver != fx->calls[j].receiver &&
          fx->calls[j].call_pc < fx->calls[i].receiver_pc &&
          fx->calls[i].receiver_pc < fx->calls[i].call_pc &&
          fx->calls[j].receiver_parent != fx->calls[j].receiver &&
          fx->calls[i].receiver_entry != (MSize)-1 &&
          fx->calls[j].receiver_entry != (MSize)-1) {
        fx->target = i;
        fx->other = j;
        break;
      }
  assert(fx->target < fx->ncalls && fx->other < fx->ncalls);
  fx->livecall = fx->ncalls;
  for (unsigned i = 0; i < fx->ncalls; i++) {
    IRRef live_ref;
    if (i != fx->other &&
        fx->calls[i].call_pc < fx->calls[fx->other].call_pc &&
        snapshot_slot(fx, fx->calls[i].snap,
                      fx->calls[fx->other].receiver_slot, NULL, &live_ref) &&
        live_ref == fx->calls[fx->other].receiver) {
      fx->livecall = i;
      break;
    }
  }
  assert(fx->livecall < fx->ncalls);
}

static void reset_fixture(TableFixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner);
  IRRef full = trace_nins_acq(fx->owner);
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk],
         (size_t)(full-nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner),
         (size_t)fx->nsnap*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         (size_t)fx->nsnapmap*sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir;
  fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  locate_calls(fx);
}

static void init_fixture(lua_State *L, jit_State *J, TableFixture *fx,
                         const char *runner)
{
  memset(fx, 0, sizeof(*fx));
  fx->J = J;
  fx->pt = global_proto(L, runner);
  fx->owner = find_trace(J, fx->pt);
  if (!fx->owner)
    fprintf(stderr, "table CALLXS trace missing for shared registry runner\n");
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

static void free_fixture(TableFixture *fx)
{
  free(fx->snapmap);
  free(fx->snap);
  free(fx->ir);
  memset(fx, 0, sizeof(*fx));
}

static JSave enter_semantic(TableFixture *fx)
{
  jit_State *J = fx->J;
  JSave save = {J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
                J->exitno, J->baseslot, J->framedepth, J->retdepth,
                J->ktrace};
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
    if (ir_load_acq(&fx->ir[r]).o == IR_LOOP) {
      J->loopref = r;
      break;
    }
  assert(J->loopref != 0);
  J->parent = 0;
  J->exitno = 0;
  J->baseslot = 1u+LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->ktrace = fx->ktrace;
  return save;
}

static void leave_semantic(jit_State *J, const JSave *s)
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

static LJArm64PostRAView postra_view(TableFixture *fx)
{
  LJArm64PostRAView view = {0};
  view.ir = fx->ir;
  view.owner = &fx->view_owner;
  view.cts = fx->cts;
  view.snap = fx->snap;
  view.snapmap = fx->snapmap;
  view.proto_bc = proto_bc(fx->pt);
  view.proto = fx->pt;
  view.startpc = trace_startpc_acq(fx->owner);
  view.nins = trace_nins_acq(fx->owner);
  view.nk = trace_nk_acq(fx->owner);
  view.nsnap = fx->nsnap;
  view.nsnapmap = fx->nsnapmap;
  view.spadjust = trace_spadjust_acq(fx->owner);
  view.proto_sizebc = fx->pt->sizebc;
  view.proto_numparams = fx->pt->numparams;
  view.root_topslot = trace_topslot_acq(fx->owner);
  view.startins = trace_startins_acq(fx->owner);
  return view;
}

static int check_semantic(TableFixture *fx)
{
  uint32_t *startpc = (uint32_t *)trace_startpc_acq(fx->owner);
  uint32_t liveins = la_load32_acq(startpc);
  IRIns tracekey = fx->ir[fx->ktrace];
  IRIns keypayload = fx->ir[fx->ktrace+1u];
  LJArm64IRReject reject;
  JSave save = enter_semantic(fx);
  int accepted;
  la_store32_rel(startpc, fx->owner->startins);
  accepted = lj_asm_arm64_ir_admit(fx->J, &fx->semantic, &reject);
  la_store32_rel(startpc, liveins);
  fx->ir[fx->ktrace] = tracekey;
  fx->ir[fx->ktrace+1u] = keypayload;
  if (!accepted)
    fprintf(stderr, "table CALLXS semantic reject reason=%d ref=%u op=%u detail=%u\n",
            (int)reject.reason, (unsigned)reject.ref, (unsigned)reject.op,
            (unsigned)reject.detail);
  leave_semantic(fx->J, &save);
  return accepted;
}

static int check_postra(TableFixture *fx)
{
  LJArm64PostRAView view = postra_view(fx);
  return lj_asm_arm64_postra_admit(&view, NULL);
}

static void replace_snapshot_ref(TableFixture *fx, TableCall *c, IRRef ref)
{
  MSize ofs = snap_mapofs_acq(&fx->snap[c->snap]);
  MSize nent = snap_nent_acq(&fx->snap[c->snap]);
  MSize i;
  for (i = 0; i < nent; i++) {
    SnapEntry *entry = &fx->snapmap[ofs+i];
    if (snap_slot(*entry) == c->cdata_slot) {
      *entry = snap_setref(*entry, ref);
      return;
    }
  }
  assert(!"CALLXS source cdata slot missing");
}

static void mutate_structural(TableFixture *fx, enum Mutation mutation)
{
  TableCall *target = &fx->calls[fx->target];
  TableCall *other = &fx->calls[fx->other];
  IRIns *ir = fx->ir;
  IRRef tailref = ir_load_acq(&ir[target->lookup]).op1;
  IRIns tail = ir_load_acq(&ir[tailref]);
  switch (mutation) {
  case M_WRONG_HELPER:
    ir[target->lookup].op2 = IRCALL_lj_tab_getscalar_rooted_try;
    break;
  case M_WRONG_STATUS:
    ir[target->status].op1 = ir_load_acq(&ir[target->status]).op2;
    break;
  case M_ROOT1_MODE:
    ir[target->root1].op2 = IRTMPREF_IN1;
    break;
  case M_ROOT2_MODE:
    ir[target->root2].op2 = IRTMPREF_IN1;
    break;
  case M_RECEIVER_GUARD: {
    IRRef source = other->receiver_parent;
    assert(source >= REF_FIRST && ir_load_acq(&ir[source]).o == IR_SLOAD &&
           (ir_load_acq(&ir[source]).t.irt & IRT_GUARD));
    ir[source].op2 = IRSLOAD_READONLY;
    break;
  }
  case M_CHAIN_RECEIVER_SOURCE:
    assert(target->receiver_root1 && other->receiver_parent);
    ir[target->receiver_root1].op1 = other->receiver_parent;
    break;
  case M_CHAIN_RECEIVER_SLOT:
    fx->snapmap[target->receiver_input_entry] = snap_setref(
      fx->snapmap[target->receiver_input_entry], other->receiver_parent);
    break;
  case M_CHAIN_RECEIVER_NORESTORE:
    fx->snapmap[target->receiver_input_entry] |= SNAP_NORESTORE;
    break;
  case M_XSAVE_TABLE_OTHER:
    fx->snapmap[target->receiver_entry] = snap_setref(
      fx->snapmap[target->receiver_entry], other->receiver);
    break;
  case M_XSAVE_TABLE_NORESTORE:
    fx->snapmap[target->receiver_entry] |= SNAP_NORESTORE;
    break;
  case M_LIVE_ROUTE_XSAVE_TABLE_OTHER: {
    TableCall *live = &fx->calls[fx->livecall];
    MSize entry;
    assert(live->receiver != other->receiver);
    assert(snapshot_slot(fx, live->snap, other->receiver_slot, &entry, NULL));
    fx->snapmap[entry] = snap_setref(fx->snapmap[entry], live->receiver);
    break;
  }
  case M_LIVE_ROUTE_XSAVE_TABLE_NORESTORE: {
    TableCall *live = &fx->calls[fx->livecall];
    MSize entry;
    assert(snapshot_slot(fx, live->snap, other->receiver_slot, &entry, NULL));
    fx->snapmap[entry] |= SNAP_NORESTORE;
    break;
  }
  case M_LIVE_ROUTE_XSAVE_TABLE_TRUNCATED: {
    TableCall *live = &fx->calls[fx->livecall];
    assert(fx->snap[live->snap].nslots > other->receiver_slot);
    fx->snap[live->snap].nslots = other->receiver_slot;
    break;
  }
  case M_CARG_OUTPUT:
    assert(tail.o == IR_CARG);
    ir[tailref].op2 = target->root2;
    break;
  case M_VLOAD_BASE:
    ir[target->value].op1 = target->root2;
    break;
  case M_VLOAD_OFFSET:
    ir[target->value].op2 = 1;
    break;
  case M_VLOAD_TYPE:
    ir[target->value].t.irt = IRT_TAB|IRT_GUARD;
    break;
  case M_VLOAD_GUARD:
    ir[target->value].t.irt = IRT_CDATA;
    break;
  case M_POSTREAD_SNAPSHOT: {
    fx->snap[target->post_snap].ref = target->value+1u;
    break;
  }
  case M_STATUS_PC:
    snapshot_set_footer_pc(fx, target->pre_snap, target->read_pc+1u);
    break;
  case M_POSTREAD_PC:
    snapshot_set_footer_pc(fx, target->post_snap, target->read_pc+1u);
    break;
  case M_RESULT_PC:
    snapshot_set_footer_pc(fx, target->after_snap, target->call_pc+1u);
    break;
  case M_POINTER_OTHER_SOURCE:
    ir[target->pointer].op1 = other->value;
    break;
  case M_CTYPE_OTHER_SOURCE:
    ir[target->ctype].op1 = other->value;
    break;
  case M_XSAVE_OTHER_CDATA:
    replace_snapshot_ref(fx, target, other->value);
    break;
  case M_XSAVE_GC_SLOT_ALIAS: {
    MSize ofs = snap_mapofs_acq(&fx->snap[target->snap]);
    MSize nent = snap_nent_acq(&fx->snap[target->snap]);
    IRRef alias = 0;
    MSize i;
    assert(ofs <= fx->nsnapmap && nent <= fx->nsnapmap-ofs);
    for (i = 0; i < nent; i++) {
      SnapEntry se = fx->snapmap[ofs+i];
      IRRef candidate = snap_ref(se);
      if (snap_slot(se) != target->cdata_slot &&
          !(se & (SNAP_NORESTORE|SNAP_FRAME)) &&
          candidate >= trace_nk_acq(fx->owner) &&
          candidate < target->xsave &&
          candidate != target->value &&
          irt_isgcv(ir_load_acq(&ir[candidate]).t)) {
        alias = candidate;
        break;
      }
    }
    assert(alias != 0);
    replace_snapshot_ref(fx, target, alias);
    break;
  }
  case M_XSAVE_NORESTORE:
    fx->snapmap[target->cdata_entry] |= SNAP_NORESTORE;
    break;
  case M_XSAVE_TRUNCATED:
    assert(fx->snap[target->snap].nslots > target->cdata_slot);
    fx->snap[target->snap].nslots = target->cdata_slot;
    break;
  default:
    assert(!"not a semantic table mutation");
  }
}

static void expect_rejection(TableFixture *fx, enum Mutation mutation,
                             const char *name)
{
  reset_fixture(fx);
  mutate_structural(fx, mutation);
  {
    int accepted = check_semantic(fx);
    if (accepted)
      fprintf(stderr, "table CALLXS semantic accepted mutation %s\n", name);
    assert(!accepted);
  }
  {
    int accepted = check_postra(fx);
    if (accepted)
      fprintf(stderr, "table CALLXS post-RA accepted mutation %s\n", name);
    assert(!accepted);
  }
}

static void expect_postra_rejection(TableFixture *fx, enum Mutation mutation,
                                    const char *name)
{
  TableCall *target;
  reset_fixture(fx);
  target = &fx->calls[fx->target];
  if (mutation == M_ROOT_ALIAS) {
    assert(ra_hasreg(ir_load_acq(&fx->ir[target->root1]).r) &&
           ra_hasreg(ir_load_acq(&fx->ir[target->root2]).r));
    fx->ir[target->root2].r = fx->ir[target->root1].r;
  } else if (mutation == M_ROOT_SPILL) {
    fx->ir[target->root1].s = SPS_FIRST;
  } else if (mutation == M_VLOAD_SPILL_HALF) {
    fx->ir[target->value].s = (uint8_t)(SPS_FIRST+1u);
  } else if (mutation == M_VLOAD_SPILL_OUTSIDE) {
    MSize capacity = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(capacity < SPS_LIMIT);
    fx->ir[target->value].s = (uint8_t)capacity;
  } else if (mutation == M_RECEIVER_SPILL_HALF) {
    fx->ir[target->receiver].s = (uint8_t)(SPS_FIRST+1u);
  } else if (mutation == M_RECEIVER_SPILL_OUTSIDE) {
    MSize capacity = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(capacity < SPS_LIMIT);
    fx->ir[target->receiver].s = (uint8_t)capacity;
  } else {
    assert(!"unexpected allocated-view mutation");
  }
  {
    int accepted = check_postra(fx);
    if (accepted)
      fprintf(stderr, "table CALLXS post-RA accepted mutation %s\n", name);
    assert(!accepted);
  }
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "table CALLXS admission setup error: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int run_driver(lua_State *L, const char *driver, lua_Integer n)
{
  lua_getglobal(L, driver);
  lua_pushinteger(L, n);
  return lua_pcall(L, 1, 1, 0);
}

static void test_function_pointer_classifier(lua_State *L)
{
  CTState *cts = ctype_ctsG(G(L));
  CType outer, function;
  CTypeID id, rawid, childid;
  LJArm64ABISignature abi;

  id = global_ctype(L, "__table_first");
  assert(id == global_ctype(L, "__table_second"));
  assert(lj_ccall_arm64_raw_type(cts, id, &outer, &rawid));
  assert(ctype_isptr(ctype_info_acq(&outer)) &&
         !(ctype_info_acq(&outer) & CTF_REF) &&
         ctype_size_acq(&outer) == sizeof(void *));
  childid = ctype_cid(ctype_info_acq(&outer));
  assert(lj_ccall_arm64_raw_type(cts, childid, &function, &rawid));
  assert(ctype_isfunc(ctype_info_acq(&function)));
  assert(lj_ccall_arm64_signature(cts, id, &abi));
  assert(abi.function_id == rawid && abi.nargs == 1 &&
         abi.arg[0].size == sizeof(int32_t));

  assert(!lj_ccall_arm64_signature(cts,
      global_ctype(L, "__table_data_pointer"), &abi));
  assert(!lj_ccall_arm64_signature(cts,
      global_ctype(L, "__table_nested_function_pointer"), &abi));
  assert(!lj_ccall_arm64_signature(cts,
      global_ctype(L, "__table_function_array"), &abi));
  {
    CTypeID refid;
    if (global_cdata_ctype(L, "__table_ref_pointer", &refid))
      assert(!lj_ccall_arm64_signature(cts, refid, &abi));
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  jit_State *J = G2J(G(L));
  TableFixture fx;
  lua_Integer expected;
  const char *path = getenv("LJ_M7_FFI_CALLXS_TABLE_SO");
  assert(path != NULL && path[0] != '\0');
  run_lua(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_callxs_table_configure(void); int32_t lj_callxs_table_first(int32_t); int32_t lj_callxs_table_second(int32_t); int32_t lj_callxs_table_first_count(void); int32_t lj_callxs_table_second_count(void);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CALLXS_TABLE_SO')))\n"
    "lib.lj_callxs_table_configure()\n"
    "_G.__table_library=lib\n"
    "local fnctype=ffi.typeof('int32_t (*)(int32_t)')\n"
    "local first=ffi.cast(fnctype,lib.lj_callxs_table_first)\n"
    "local second=ffi.cast(fnctype,lib.lj_callxs_table_second)\n"
    "_G.__table_data_pointer=ffi.cast('int32_t *',0)\n"
    "_G.__table_nested_function_pointer=ffi.cast('int32_t (**)(int32_t)',0)\n"
    "_G.__table_function_array=ffi.new('int32_t[4]')\n"
    "local refok,refvalue=pcall(function() return ffi.cast('int32_t (*)(int32_t) &',0) end)\n"
    "if refok then _G.__table_ref_pointer=refvalue end\n"
    "_G.__table_first,_G.__table_second=first,second\n"
    "_G.__table_first_count,_G.__table_second_count=lib.lj_callxs_table_first_count,lib.lj_callxs_table_second_count\n"
    "_G.__table_registry={call=first,[true]=second}\n"
    "_G.__table_chain_registry={routes={call=first,child={call=second}},other={call=first}}\n"
    "_G.__table_chain_ready=threading.channel(1); _G.__table_chain_stop=threading.channel(1)\n"
    "_G.__table_chain_peer=threading.spawn(function(reg,ready,stop)\n"
    "  assert(ready:send(true,5)); local _,ok=stop:recv(30); assert(ok); return reg.routes.child.call~=nil\n"
    "end,__table_chain_registry,__table_chain_ready,__table_chain_stop)\n"
    "local _,ready_ok=__table_chain_ready:recv(5); assert(ready_ok)\n"
    "function _G.__table_chain_run(reg,n)\n"
    "  local sum=0.5\n"
    "  for i=1,n do\n"
    "    local route=reg.routes; local alt=reg.other; sum=sum+alt.call(0)\n"
    "    sum=sum+route.call(i); local child=route.child\n"
    "    sum=sum+child.call(i); sum=sum+child.call(0)\n"
    "  end\n"
    "  return sum-0.5\n"
    "end\n"
    "function _G.__table_chain_driver(n)\n"
    "  local result=__table_chain_run(__table_chain_registry,n); return result\n"
    "end\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=32')\n");
  run_lua(L, "if os.getenv('LJ_ARM64_TABLE_CALL_DUMP') then "
             "require('jit.dump').on('isaT') end");
  expected = 4*32*33/2 + 236*32;
  assert(run_driver(L, "__table_chain_driver", 32) == LUA_OK && lua_isnumber(L, -1) &&
         lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
  run_lua(L, "assert(__table_first_count()==64 and __table_second_count()==64)");
  init_fixture(L, J, &fx, "__table_chain_run");
  assert(check_semantic(&fx));
  assert(check_postra(&fx));
  test_function_pointer_classifier(L);

  expect_rejection(&fx, M_WRONG_HELPER, "wrong lookup helper");
  expect_rejection(&fx, M_WRONG_STATUS, "lookup status not positive");
  expect_rejection(&fx, M_ROOT1_MODE, "result root mode");
  expect_rejection(&fx, M_ROOT2_MODE, "key root mode");
  expect_rejection(&fx, M_RECEIVER_GUARD,
                   "table SLOAD missing type-check flag");
  expect_rejection(&fx, M_CARG_OUTPUT, "CARG result root alias");
  expect_rejection(&fx, M_VLOAD_BASE, "VLOAD from wrong root");
  expect_rejection(&fx, M_VLOAD_OFFSET, "VLOAD nonzero TValue offset");
  expect_rejection(&fx, M_VLOAD_TYPE, "VLOAD wrong TValue type");
  expect_rejection(&fx, M_VLOAD_GUARD, "VLOAD missing type guard");
  expect_rejection(&fx, M_CHAIN_RECEIVER_SOURCE,
                   "nested table receiver uses unrelated SLOAD");
  expect_rejection(&fx, M_CHAIN_RECEIVER_SLOT,
                   "nested table receiver snapshot names unrelated table");
  expect_rejection(&fx, M_CHAIN_RECEIVER_NORESTORE,
                   "nested receiver table is not restored");
  expect_rejection(&fx, M_XSAVE_TABLE_OTHER,
                   "XSAVE uses unrelated nested receiver table");
  expect_rejection(&fx, M_XSAVE_TABLE_NORESTORE,
                   "XSAVE nested receiver table is not restored");
  expect_rejection(&fx, M_LIVE_ROUTE_XSAVE_TABLE_OTHER,
                   "first native XSAVE substitutes another table root");
  expect_rejection(&fx, M_LIVE_ROUTE_XSAVE_TABLE_NORESTORE,
                   "first native XSAVE omits the live route table");
  expect_rejection(&fx, M_LIVE_ROUTE_XSAVE_TABLE_TRUNCATED,
                   "first native XSAVE truncates the live route table");
  expect_rejection(&fx, M_POSTREAD_SNAPSHOT, "post-read snapshot moved");
  expect_rejection(&fx, M_STATUS_PC,
                   "lookup refusal snapshot resumes after TGETS");
  expect_rejection(&fx, M_POSTREAD_PC,
                   "VLOAD guard snapshot resumes after TGETS");
  expect_rejection(&fx, M_RESULT_PC,
                   "post-VLOAD result snapshot resumes after CALL");
  expect_rejection(&fx, M_POINTER_OTHER_SOURCE,
                   "pointer FLOAD from other same-type CData");
  expect_rejection(&fx, M_CTYPE_OTHER_SOURCE,
                   "CTYPEID FLOAD from other same-type CData");
  expect_rejection(&fx, M_XSAVE_OTHER_CDATA,
                   "XSAVE uses other same-type CData");
  expect_rejection(&fx, M_XSAVE_GC_SLOT_ALIAS,
                   "XSAVE CData aliases another live GC slot");
  expect_rejection(&fx, M_XSAVE_NORESTORE, "XSAVE CData not restored");
  expect_rejection(&fx, M_XSAVE_TRUNCATED, "XSAVE GC TValue span truncated");
  expect_postra_rejection(&fx, M_ROOT_ALIAS, "root/key register alias");
  expect_postra_rejection(&fx, M_ROOT_SPILL, "root scratch spilled");
  expect_postra_rejection(&fx, M_VLOAD_SPILL_HALF,
                          "CData VLOAD spilled at half-slot alignment");
  expect_postra_rejection(&fx, M_VLOAD_SPILL_OUTSIDE,
                          "CData VLOAD spill exceeds frame capacity");
  expect_postra_rejection(&fx, M_RECEIVER_SPILL_HALF,
                          "nested table VLOAD spill at half-slot alignment");
  expect_postra_rejection(&fx, M_RECEIVER_SPILL_OUTSIDE,
                          "nested table VLOAD spill exceeds frame capacity");
  run_lua(L,
    "assert(__table_chain_stop:send(true,5))\n"
    "local ok,result=__table_chain_peer:join(5); assert(ok and result)\n");
  free_fixture(&fx);
  lua_close(L);
  puts("t-arm64-jit-table-chains-admission OK: nested table source proofs");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-table-chains-admission SKIP: requires ARM64 experimental CALLXS");
  return 0;
}

#endif
