/* Independent semantic and post-RA admission for owned int32/uint32 CData
** CALLXS arguments. */
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

#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
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
#error "CData32 admission requires ARM64 CALLXS admission"
#endif

enum { KINDS = 2, SOURCES = 2, FORMS = 2, SHAPES = KINDS*SOURCES*FORMS };
static const char *const kind_name[KINDS] = {"i32", "u32"};

typedef struct Fixture {
  lua_State *L;
  jit_State *J;
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nk, nins, ktrace;
  IRRef call, xsave, carrier, source, carg_node;
  IRRef values[16], nodes[16];
  unsigned nargs, kind, table_source, stack;
  BCReg arg_slot;
  MSize nsnap, nsnapmap, xsnap, arg_entry;
  GCtrace view_owner, semantic;
} Fixture;

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
  M_XSAVE_WRONG_OWNER, M_XSAVE_NORESTORE, M_XSAVE_TRUNCATED,
  M_PAYLOAD_FIELD, M_PAYLOAD_TAG, M_PAYLOAD_OWNER,
  M_CTYPE_GUARD_MISSING, M_CTYPE_GUARD_WRONG,
  M_SIGNATURE_WIDTH, M_SIGNATURE_SIGN, M_RAW_SNAPSHOT, M_UNRELATED_INT_USE,
  M_CARRIER_WRONG_BANK, M_CARRIER_SPILL_EXTENT
};

static unsigned semantic_ok, semantic_bad, postra_ok, postra_bad;

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

static IRRef find_ktrace(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef r, nk = trace_nk_acq((GCtrace *)T);
  for (r = nk; r < REF_TRUE;) {
    IRIns k = ir_load_acq(&ir[r]);
    if (k.o == IR_KGC && ir_kgc_load_acq(&ir[r]) == obj2gco(T)) return r;
    if (k.o == IR_KNUM || k.o == IR_KINT64 ||
        (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KKPTR)))
      r += 2u;
    else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
             k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL)
      r++;
    else return 0;
  }
  return 0;
}

static IRRef semantic_nins(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq((GCtrace *)T);
  IRRef n = trace_nins_acq((GCtrace *)T);
  while (n > REF_BASE) {
    IRIns x = ir_load_acq(&ir[n-1u]);
    if (x.o == IR_RENAME && x.t.irt == IRT_NIL && x.op1 >= REF_FIRST &&
        x.op1 < n-1u && x.op2 < trace_nsnap_acq((GCtrace *)T) &&
        x.s == SPS_NONE) n--;
    else if (x.o == IR_NOP && x.t.irt == IRT_NIL &&
             !x.op1 && !x.op2 && !x.prev) n--;
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
        trace_startpt_acq(T) == pt && bc_op(trace_startins_acq(T)) == BC_FORL &&
        op_count(T, IR_CALLXS) &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE)) return T;
  }
  return NULL;
}

static int snap_slot_ref(Fixture *fx, MSize sn, BCReg slot,
                         MSize *entryp, IRRef *refp)
{
  MSize ofs, n, i;
  unsigned found = 0;
  if (sn >= fx->nsnap) return 0;
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  n = snap_nent_acq(&fx->snap[sn]);
  if (ofs > fx->nsnapmap || n > fx->nsnapmap-ofs) return 0;
  for (i = 0; i < n; i++) {
    SnapEntry e = fx->snapmap[ofs+i];
    if (snap_slot(e) == slot) {
      if (found++) return 0;
      if (entryp) *entryp = ofs+i;
      if (refp) *refp = snap_ref(e);
    }
  }
  return found == 1;
}

static int snap_for_ref(Fixture *fx, IRRef ref, MSize *out)
{
  MSize i;
  for (i = 0; i < fx->nsnap; i++)
    if (snap_ref_acq(&fx->snap[i]) == ref) { *out = i; return 1; }
  return 0;
}

static const BCIns *source_ins(Fixture *fx, const BCIns *pc, IRRef xsave,
                               const BCIns **returnpc)
{
  IRIns mark = ir_load_acq(&fx->ir[xsave]);
  uint32_t offset = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
  const BCIns *bc;
  assert(mark.o == IR_XSAVE && offset > 0 && offset < fx->pt->sizebc);
  bc = pc + offset - 1u;
  if (returnpc) *returnpc = pc + offset;
  return bc;
}

static unsigned carg_values(Fixture *fx, IRRef call, IRRef *values,
                            IRRef *nodes, unsigned cap)
{
  IRRef rev[32], rnodes[32], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  for (;;) {
    IRIns node;
    if (n >= cap || r < fx->nk || r >= call) return 0;
    node = ir_load_acq(&fx->ir[r]);
    if (node.o != IR_CARG) {
      rev[n] = r; rnodes[n++] = 0; break;
    }
    if (node.t.irt != IRT_NIL || node.op1 >= r || node.op2 >= r) return 0;
    rev[n] = node.op2; rnodes[n++] = r; r = node.op1;
  }
  for (i = 0; i < n; i++) values[i] = rev[n-i-1u], nodes[i] = rnodes[n-i-1u];
  return n;
}

static void reset_fixture(Fixture *fx)
{
  IRRef full = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, ((size_t)full+1u)*sizeof(IRIns));
  memcpy(&fx->ir[fx->nk], &trace_ir_acq(fx->owner)[fx->nk],
         (size_t)(full-fx->nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner), fx->nsnap*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner),
         fx->nsnapmap*sizeof(SnapEntry));
  fx->view_owner = *fx->owner;
  fx->view_owner.ir = fx->ir; fx->view_owner.snap = fx->snap;
  fx->view_owner.snapmap = fx->snapmap;
  fx->nins = semantic_nins(fx->owner);
  fx->call = fx->xsave = 0;
  {
    IRRef r;
    for (r = REF_FIRST; r < fx->nins; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_CALLXS) {
        IRRef x;
        fx->call = r;
        for (x = r; x > REF_FIRST;) {
          IRRef p = --x;
          if (ir_load_acq(&fx->ir[p]).o == IR_XSAVE) {
            fx->xsave = p; break;
          }
        }
      }
    }
  }
  assert(fx->call && fx->xsave);
  fx->nargs = carg_values(fx, fx->call, fx->values, fx->nodes, 16);
  assert(fx->nargs == (fx->stack ? 12u : 1u));
  fx->carrier = fx->values[fx->nargs-1u];
  {
    IRIns value = ir_load_acq(&fx->ir[fx->carrier]);
    assert(value.o == IR_FLOAD && value.op2 == IRFL_CDATA_INT &&
           value.t.irt == (fx->kind ? IRT_U32 : IRT_INT));
    fx->source = value.op1;
    assert(fx->source < fx->carrier && fx->source >= fx->nk);
    {
      IRIns owner = ir_load_acq(&fx->ir[fx->source]);
      assert(owner.o == (fx->table_source ? IR_VLOAD : IR_SLOAD));
    }
  }
  assert(snap_for_ref(fx, fx->xsave, &fx->xsnap));
  {
    const BCIns *retpc = NULL;
    BCIns src = *source_ins(fx, proto_bc(fx->pt), fx->xsave, &retpc);
    IRIns mark = ir_load_acq(&fx->ir[fx->xsave]);
    uint32_t off = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
    UNUSED(src); UNUSED(retpc);
    assert(bc_op(src) == BC_CALL && bc_b(src) != 0);
    fx->arg_slot = (BCReg)(bc_a(src) + 3u + 2u*LJ_FR2 +
                              (fx->stack ? 11u : 0u));
    {
      IRRef staged;
      assert(snap_slot_ref(fx, fx->xsnap, fx->arg_slot,
                           &fx->arg_entry, &staged));
      assert(staged == fx->source);
    }
    UNUSED(off);
  }
  assert(fx->source != 0);
}

static int semantic_accept(Fixture *fx)
{
  jit_State *J = fx->J;
  uint32_t *start = (uint32_t *)(uintptr_t)trace_startpc_acq(fx->owner);
  uint32_t old = la_load32_acq(start);
  JSave save = {J->curfinal, J->pt, J->startpc, J->loopref, J->parent,
    J->exitno, J->baseslot, J->framedepth, J->retdepth, J->ktrace};
  int got;
  la_store32_rel(start, fx->owner->startins);
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
  assert(J->loopref);
  J->parent = 0; J->exitno = 0; J->baseslot = 1u+LJ_FR2;
  J->framedepth = 0; J->retdepth = 0; J->ktrace = fx->ktrace;
  got = lj_asm_arm64_ir_admit(J, &fx->semantic, NULL);
  J->curfinal=save.curfinal; J->pt=save.pt; J->startpc=save.startpc;
  J->loopref=save.loopref; J->parent=save.parent; J->exitno=save.exitno;
  J->baseslot=save.baseslot; J->framedepth=save.framedepth;
  J->retdepth=save.retdepth; J->ktrace=save.ktrace;
  la_store32_rel(start, old);
  return got;
}

static int postra_accept(Fixture *fx)
{
  LJArm64PostRAView v = {0};
  v.ir=fx->ir; v.owner=fx->owner; v.cts=fx->cts;
  v.snap=fx->snap; v.snapmap=fx->snapmap; v.proto_bc=proto_bc(fx->pt);
  v.proto=fx->pt; v.startpc=trace_startpc_acq(fx->owner);
  v.nins=trace_nins_acq(&fx->view_owner); v.nk=fx->nk;
  v.nsnap=fx->nsnap; v.nsnapmap=fx->nsnapmap;
  v.spadjust=trace_spadjust_acq(fx->owner); v.proto_sizebc=fx->pt->sizebc;
  v.proto_numparams=fx->pt->numparams; v.root_topslot=trace_topslot_acq(fx->owner);
  v.startins=trace_startins_acq(fx->owner);
  return lj_asm_arm64_postra_admit(&v, NULL);
}

static IRRef other_cdata(Fixture *fx)
{
  IRRef r;
  IRIns fp = ir_load_acq(&fx->ir[ir_load_acq(&fx->ir[fx->call]).op2]);
  IRRef callable = fp.op1;
  for (r = REF_FIRST; r < fx->carrier; r++) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (r != fx->source && r != callable &&
        (in.o == IR_SLOAD || in.o == IR_VLOAD) &&
        in.t.irt == (IRT_CDATA|IRT_GUARD)) return r;
  }
  /* The callable CData is a valid same-IR-tag alternate owner. */
  return callable;
}

static void wrong_raw_snapshot(Fixture *fx)
{
  MSize s, i;
  for (s = 0; s < fx->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&fx->snap[s]);
    MSize n = snap_nent_acq(&fx->snap[s]);
    IRRef at = snap_ref_acq(&fx->snap[s]);
    if (at <= fx->carrier || ofs > fx->nsnapmap || n > fx->nsnapmap-ofs) continue;
    for (i = 0; i < n; i++) {
      SnapEntry e = fx->snapmap[ofs+i];
      IRRef old = snap_ref(e);
      BCIns src = *source_ins(fx, proto_bc(fx->pt), fx->xsave, NULL);
      if (snap_slot(e) < bc_a(src) && e == SNAP(snap_slot(e),0,old) &&
          old >= fx->nk && old < at && old != fx->source &&
          irt_type(ir_load_acq(&fx->ir[old]).t) == IRT_INT) {
        fx->snapmap[ofs+i] = snap_setref(e, fx->carrier);
        return;
      }
    }
  }
  assert(!"no unrelated INT snapshot slot for raw CData carrier leak");
}

static void mutate(Fixture *fx, enum Mutation m)
{
  IRIns *payload = &fx->ir[fx->carrier];
  IRRef other = other_cdata(fx);
  switch (m) {
  case M_XSAVE_WRONG_OWNER:
    if (fx->arg_entry != (MSize)-1)
      fx->snapmap[fx->arg_entry] = snap_setref(fx->snapmap[fx->arg_entry], other);
    else
      payload->op1 = other;
    break;
  case M_XSAVE_NORESTORE:
    assert(fx->arg_entry != (MSize)-1);
    fx->snapmap[fx->arg_entry] |= SNAP_NORESTORE;
    break;
  case M_XSAVE_TRUNCATED:
    fx->snap[fx->xsnap].nslots = fx->arg_slot;
    break;
  case M_PAYLOAD_FIELD:
    payload->op2 = IRFL_CDATA_PTR;
    break;
  case M_PAYLOAD_TAG:
    payload->t.irt = fx->kind ? IRT_INT : IRT_U32;
    break;
  case M_PAYLOAD_OWNER:
    payload->op1 = other;
    break;
  case M_CTYPE_GUARD_MISSING:
  case M_CTYPE_GUARD_WRONG: {
    IRRef r, field = 0, guard = 0;
    for (r = fx->source+1u; r < fx->carrier; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_FLOAD && in.op1 == fx->source &&
          in.op2 == IRFL_CDATA_CTYPEID) { field = r; break; }
    }
    assert(field);
    for (r = field+1u; r < fx->carrier; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_EQ && (in.op1 == field || in.op2 == field)) { guard = r; break; }
    }
    assert(guard);
    if (m == M_CTYPE_GUARD_MISSING) fx->ir[guard].o = IR_NE;
    else {
      CTypeID id = global_ctype(fx->L, "__cdata32_other_value");
      IRIns eq = ir_load_acq(&fx->ir[guard]);
      IRRef k = eq.op1 == field ? eq.op2 : eq.op1;
      assert(k >= fx->nk && fx->ir[k].o == IR_KINT);
      fx->ir[k].i = (int32_t)id;
    }
    break;
  }
  case M_SIGNATURE_WIDTH:
  case M_SIGNATURE_SIGN: {
    IRIns fn = ir_load_acq(&fx->ir[fx->call]);
    IRIns ptr = ir_load_acq(&fx->ir[fn.op2]);
    IRRef callable = ptr.op1, field = 0, guard = 0;
    const char *alternate = m == M_SIGNATURE_SIGN
      ? (fx->stack ? (fx->kind ? "__cdata32_stack_i32" : "__cdata32_stack_u32")
                   : (fx->kind ? "__cdata32_i32" : "__cdata32_u32"))
      : (fx->stack ? "__cdata32_stack_i64_fn" : "__cdata32_i64_fn");
    CTypeID alt = global_ctype(fx->L, alternate);
    for (IRRef r = REF_FIRST; r < fx->call; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_FLOAD && in.op1 == callable && in.op2 == IRFL_CDATA_CTYPEID) {
        field = r; break;
      }
    }
    assert(field);
    for (IRRef r = field+1u; r < fx->call; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_EQ && (in.op1 == field || in.op2 == field)) { guard=r; break; }
    }
    assert(guard);
    IRIns eq = ir_load_acq(&fx->ir[guard]);
    IRRef k = eq.op1 == field ? eq.op2 : eq.op1;
    assert(fx->ir[k].o == IR_KINT);
    fx->ir[k].i = (int32_t)alt;
    break;
  }
  case M_RAW_SNAPSHOT:
    wrong_raw_snapshot(fx);
    break;
  case M_UNRELATED_INT_USE: {
    IRRef r;
    assert(fx->kind == 0 && payload->t.irt == IRT_INT);
    /* Use the payload in the existing Lua loop induction expression. Its
    ** output already feeds LE/PHI: no orphan node or invalid type is added. */
    for (r = fx->call+1u; r < fx->nins; r++) {
      IRIns in = ir_load_acq(&fx->ir[r]);
      if (in.o == IR_ADD && irt_type(in.t) == IRT_INT &&
          in.op2 >= fx->nk && in.op2 < REF_FIRST &&
          fx->ir[in.op2].o == IR_KINT && fx->ir[in.op2].i == 1 &&
          in.op1 != fx->carrier &&
          irt_type(ir_load_acq(&fx->ir[in.op1]).t) == IRT_INT) {
        fx->ir[r].op1 = fx->carrier;
        break;
      }
    }
    assert(r < fx->nins);
    break;
  }
  case M_CARRIER_WRONG_BANK:
    payload->r = RID_D0; payload->s = SPS_NONE;
    break;
  case M_CARRIER_SPILL_EXTENT: {
    MSize cap = SPS_FIXED + trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(cap < UINT8_MAX);
    payload->r=RID_NONE; payload->s=(uint8_t)cap;
    break;
  }
  }
}

static void expect_both(Fixture *fx, enum Mutation m)
{
  reset_fixture(fx);
  assert(semantic_accept(fx));
  reset_fixture(fx);
  assert(postra_accept(fx));
  reset_fixture(fx); mutate(fx,m);
  if (semantic_accept(fx)) {
    fprintf(stderr,"CData32 semantic accepted mutation %d kind%u table%u stack%u\n",
      m,fx->kind,fx->table_source,fx->stack); abort();
  }
  semantic_bad++;
  reset_fixture(fx); mutate(fx,m);
  if (postra_accept(fx)) {
    fprintf(stderr,"CData32 postRA accepted mutation %d kind%u table%u stack%u\n",
      m,fx->kind,fx->table_source,fx->stack); abort();
  }
  postra_bad++;
}

static void capture(Fixture *fx, lua_State *L, jit_State *J,
                    const char *runner, unsigned kind, unsigned table,
                    unsigned stack)
{
  fx->L=L; fx->J=J; fx->cts=ctype_ctsG(G(L)); fx->kind=kind;
  fx->table_source=table; fx->stack=stack; fx->pt=global_proto(L,runner);
  fx->owner=find_trace(J,fx->pt); assert(fx->owner);
  fx->nk=trace_nk_acq(fx->owner); fx->nins=semantic_nins(fx->owner);
  fx->ktrace=find_ktrace(fx->owner); fx->nsnap=trace_nsnap_acq(fx->owner);
  fx->nsnapmap=trace_nsnapmap_acq(fx->owner);
  assert(fx->cts && fx->ktrace && fx->nsnap && fx->nsnapmap);
  fx->ir=(IRIns *)calloc(trace_nins_acq(fx->owner)+1u,sizeof(IRIns));
  fx->snap=(SnapShot *)calloc(fx->nsnap,sizeof(SnapShot));
  fx->snapmap=(SnapEntry *)calloc(fx->nsnapmap,sizeof(SnapEntry));
  assert(fx->ir && fx->snap && fx->snapmap);
  reset_fixture(fx);
}

static void free_fixture(Fixture *fx)
{ free(fx->snapmap); free(fx->snap); free(fx->ir); }

static void install_and_record(lua_State *L, jit_State *J, unsigned kind,
                               unsigned table, unsigned stack)
{
  char id[64], runner[80];
  snprintf(id,sizeof(id),"k%u_src%u_%s",kind,table,stack?"stack":"unary");
  lua_getglobal(L,"__cdata32_install"); lua_pushstring(L,id);
  lua_pushinteger(L,kind); lua_pushboolean(L,table); lua_pushboolean(L,stack);
  assert(lua_pcall(L,4,0,0)==LUA_OK);
  snprintf(runner,sizeof(runner),"__cdata32_run_%s",id);
  ljt_lua_dostring(L,"jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')");
  lua_getglobal(L,"__cdata32_configure"); lua_pushinteger(L,(lua_Integer)kind);
  lua_pushboolean(L,stack); assert(lua_pcall(L,2,0,0)==LUA_OK);
  lua_getglobal(L,"__cdata32_set_value"); lua_pushinteger(L,(lua_Integer)kind);
  lua_pushinteger(L,kind ? (lua_Integer)UINT32_C(0xdeadbeef) : -17);
  assert(lua_pcall(L,2,0,0)==LUA_OK);
  for (unsigned attempt=0; attempt<8 && !find_trace(J,global_proto(L,runner)); attempt++) {
    lua_getglobal(L,"__cdata32_driver"); lua_getglobal(L,runner);
    lua_getglobal(L,table?"__cdata32_table":"__cdata32_table_value");
    snprintf(id,sizeof(id),"__cdata32_%s%s",stack?"stack_":"",kind_name[kind]);
    lua_getglobal(L,id); lua_pushinteger(L,24);
    assert(lua_pcall(L,4,1,0)==LUA_OK && lua_isnumber(L,-1)); lua_pop(L,1);
  }
  assert(find_trace(J,global_proto(L,runner)));
}

static void test_shape(lua_State *L, jit_State *J, unsigned kind,
                       unsigned table, unsigned stack)
{
  Fixture fx={0}; char id[64], runner[80];
  snprintf(id,sizeof(id),"k%u_src%u_%s",kind,table,stack?"stack":"unary");
  snprintf(runner,sizeof(runner),"__cdata32_run_%s",id);
  install_and_record(L,J,kind,table,stack);
  capture(&fx,L,J,runner,kind,table,stack);
  assert(semantic_accept(&fx)); semantic_ok++;
  reset_fixture(&fx);
  assert(postra_accept(&fx)); postra_ok++;
  expect_both(&fx,M_XSAVE_WRONG_OWNER);
  expect_both(&fx,M_XSAVE_TRUNCATED);
  expect_both(&fx,M_PAYLOAD_FIELD);
  expect_both(&fx,M_PAYLOAD_TAG);
  expect_both(&fx,M_PAYLOAD_OWNER);
  expect_both(&fx,M_CTYPE_GUARD_MISSING);
  expect_both(&fx,M_CTYPE_GUARD_WRONG);
  expect_both(&fx,M_SIGNATURE_WIDTH);
  expect_both(&fx,M_SIGNATURE_SIGN);
  expect_both(&fx,M_XSAVE_NORESTORE);
  expect_both(&fx,M_RAW_SNAPSHOT);
  if (kind == 0) expect_both(&fx,M_UNRELATED_INT_USE);
  reset_fixture(&fx);
  mutate(&fx,M_CARRIER_WRONG_BANK);
  assert(!postra_accept(&fx)); postra_bad++;
  reset_fixture(&fx);
  mutate(&fx,M_CARRIER_SPILL_EXTENT);
  assert(!postra_accept(&fx)); postra_bad++;
  free_fixture(&fx);
}

int main(void)
{
  lua_State *L=ljt_lua_newstate_openlibs(); jit_State *J=G2J(G(L));
  const char *so=getenv("LJ_M7_FFI_CDATA32_ARGS_SO");
  assert(so && *so);
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local th=require('threading')\n"
    "ffi.cdef[[void lj_ffi_cdata32_configure(int32_t,int32_t); "
    "double lj_ffi_cdata32_i32(int32_t); double lj_ffi_cdata32_u32(uint32_t); "
    "double lj_ffi_cdata32_stack_i32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int32_t); "
    "double lj_ffi_cdata32_stack_u32(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,uint32_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_CDATA32_ARGS_SO')))\n"
    "_G.__cdata32_i32=lib.lj_ffi_cdata32_i32; _G.__cdata32_u32=lib.lj_ffi_cdata32_u32; "
    "_G.__cdata32_stack_i32=lib.lj_ffi_cdata32_stack_i32; _G.__cdata32_stack_u32=lib.lj_ffi_cdata32_stack_u32; "
    "_G.__cdata32_other_value=ffi.new('int32_t',7); "
    "function __cdata32_configure(k,s) lib.lj_ffi_cdata32_configure(k,s and 1 or 0) end\n"
    "_G.__cdata32_table={}; _G.__cdata32_table_value=nil; "
    "_G.__cdata32_i64_fn=ffi.cast('double(*)(int64_t)',lib.lj_ffi_cdata32_i32); "
    "_G.__cdata32_stack_i64_fn=ffi.cast('double(*)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,uint8_t,int16_t,uint32_t,int64_t)',lib.lj_ffi_cdata32_stack_i32); "
    "function __cdata32_set_value(k,v) local ty=k==0 and 'int32_t' or 'uint32_t'; local x=ffi.new(ty,v); "
    "__cdata32_table.value=x; __cdata32_table_value=x; __cdata32_other_value=ffi.new(k==0 and 'uint32_t' or 'int32_t',7); "
    "__cdata32_other_fn=k==0 and lib.lj_ffi_cdata32_u32 or lib.lj_ffi_cdata32_i32 end\n"
    "function __cdata32_install(id,k,table_source,stack) local x=table_source and 'arg.value' or 'arg'; "
    "local c='__cdata32_'..(stack and 'stack_' or '')..(k==0 and 'i32' or 'u32'); "
    "local a=stack and ('101,102,103,104,105,106,107,108,31,-505,1985229328,'..x) or x; "
    "local body='local sum=.5; for i=1,n do sum=sum+c('..a..') end; return sum-.5'; "
    "local src='return function(arg,c,n) '..body..' end'; _G['__cdata32_run_'..id]=assert(loadstring(src,'@cdata32-'..id))() end\n"
    "function __cdata32_driver(run,arg,c,n) local r=run(arg,c,n); return r end\n"
    "__cdata32_ready=th.channel(1); __cdata32_stop=th.channel(1); "
    "__cdata32_peer=assert(th.spawn(function(held,a,b) assert(a:send(true,5)); "
    "assert(select(2,b:recv(60))); return true end,__cdata32_table,__cdata32_ready,__cdata32_stop)); "
    "assert(select(2,__cdata32_ready:recv(5))); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=512')\n");
  for (unsigned kind=0; kind<KINDS; kind++)
    for (unsigned source=0; source<SOURCES; source++)
      for (unsigned stack=0; stack<FORMS; stack++)
        test_shape(L,J,kind,source,stack);
  ljt_lua_dostring(L,"assert(__cdata32_stop:send(true,5)); local ok,v=__cdata32_peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("CData32 CALLXS admission semantic=%u/%u postRA=%u/%u\n",
         semantic_ok,semantic_bad,postra_ok,postra_bad);
  return 0;
}
#else
int main(void) { puts("t-arm64-jit-ffi-cdata32-args-admission SKIP"); return 0; }
#endif
