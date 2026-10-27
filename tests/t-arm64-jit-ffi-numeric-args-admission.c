/* Independent semantic and post-RA proofs for Lua NUM -> C integer CALLXS. */

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
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if !LJ_HASJIT_FFI_CALLXS
#error "numeric CALLXS admission requires ARM64 CALLXS admission"
#endif

enum { CASES = 9, RUNS = 24 };

typedef struct NumCase {
  const char *runner, *callee;
  unsigned arg, kind, width, is_unsigned;
} NumCase;

typedef struct Fixture {
  lua_State *L;
  jit_State *J;
  CTState *cts;
  GCproto *pt;
  GCtrace *owner;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, ktrace, call, source, carrier, chain[4], xsave;
  IRRef helper, status, root1, root2;
  unsigned nchain;
  MSize nsnap, nsnapmap, xsnap, arg_entry;
  BCReg arg_slot;
  GCtrace view_owner, semantic;
  NumCase spec;
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
  M_NUM_SOURCE, M_NUM_MODE, M_NUM_TYPE, M_FINAL_TYPE, M_NARROW_MODE,
  M_ABI_SIGN, M_XSAVE_SOURCE, M_XSAVE_INTERMEDIATE, M_XSAVE_NORESTORE,
  M_XSAVE_TRUNCATED, M_BAD_BANK, M_BAD_SPILL_ALIGN, M_BAD_SPILL_EXTENT,
  M_STACK_WRONG_CARRIER
};

static unsigned positives, semantic_rejects, postra_rejects;

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
             !tail.op1 && !tail.op2 && !tail.prev) n--;
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
        op_count(T, IR_CALLXS) >= 1u &&
        op_count(T, IR_CALLXS) == op_count(T, IR_XSAVE)) return T;
  }
  return NULL;
}

static int snapshot_slot(Fixture *fx, MSize sn, BCReg slot,
                         MSize *entryp, IRRef *refp)
{
  MSize ofs, nent, i;
  unsigned found = 0;
  if (sn >= fx->nsnap) return 0;
  ofs = snap_mapofs_acq(&fx->snap[sn]);
  nent = snap_nent_acq(&fx->snap[sn]);
  if (ofs > fx->nsnapmap || nent > fx->nsnapmap-ofs) return 0;
  for (i = 0; i < nent; i++) {
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
  MSize s;
  for (s = 0; s < fx->nsnap; s++)
    if (snap_ref_acq(&fx->snap[s]) == ref) { *out = s; return 1; }
  return 0;
}

static int chain_reaches(Fixture *fx, IRRef from, IRRef source,
                         IRRef *out, unsigned *n, unsigned depth)
{
  IRIns in;
  if (from == source) return 1;
  if (depth == 0 || from < REF_FIRST || from >= trace_nins_acq(fx->owner)) return 0;
  in = ir_load_acq(&fx->ir[from]);
  if (in.o != IR_CONV || in.op1 >= from) return 0;
  if (chain_reaches(fx, in.op1, source, out, n, depth-1u)) {
    out[(*n)++] = from;
    return 1;
  }
  return 0;
}

static unsigned carg_values(Fixture *fx, IRRef call, IRRef *values, unsigned cap)
{
  IRRef rev[32], r = ir_load_acq(&fx->ir[call]).op1;
  unsigned n = 0, i;
  while (r >= REF_FIRST && r < call) {
    IRIns in = ir_load_acq(&fx->ir[r]);
    if (in.o != IR_CARG) break;
    if (in.t.irt != IRT_NIL || n+1u >= cap || in.op1 >= r || in.op2 >= r) return 0;
    rev[n++] = in.op2;
    r = in.op1;
  }
  if (r < trace_nk_acq(fx->owner) || r >= call || n >= cap) return 0;
  values[0] = r;
  for (i = 0; i < n; i++) values[i+1u] = rev[n-i-1u];
  return n+1u;
}

static int locate_call(Fixture *fx)
{
  IRRef values[32], r, value = 0, call = 0;
  unsigned count, arg = fx->spec.arg;
  IRIns ins;
  for (r = REF_FIRST; r < trace_nins_acq(fx->owner); r++) {
    IRRef call_value = 0, call_helper = 0, call_status = 0;
    IRRef call_chain[4] = {0}; unsigned call_nchain = 0;
    ins = ir_load_acq(&fx->ir[r]);
    if (ins.o != IR_CALLXS) continue;
    count = carg_values(fx, r, values, 31);
    if (count <= arg) continue;
    for (IRRef v = REF_FIRST; v+1u < r; v++) {
      IRIns helper = ir_load_acq(&fx->ir[v]);
      IRIns status = ir_load_acq(&fx->ir[v+1u]);
      IRIns load = ir_load_acq(&fx->ir[v+2u]);
      IRRef chain[4] = {0}; unsigned nchain = 0;
      if (helper.o != IR_CALLS ||
          (helper.op2 != IRCALL_lj_tab_getscalar_rooted_try &&
           helper.op2 != IRCALL_lj_tab_gettv_rooted_hit_try) ||
          load.o != IR_VLOAD || !irt_isguard(load.t) ||
          irt_type(load.t) != IRT_NUM ||
          !chain_reaches(fx, values[arg], v+2u, chain, &nchain, 3u)) continue;
      if (call_value) return 0;
      call_value = v+2u; call_helper = v; call_status = v+1u;
      call_nchain = nchain;
      memcpy(call_chain, chain, nchain*sizeof(IRRef));
      UNUSED(status);
    }
    if (call_value) {
      value = call_value; call = r; fx->helper = call_helper;
      fx->status = call_status; fx->nchain = call_nchain;
      memcpy(fx->chain, call_chain, call_nchain*sizeof(IRRef));
      break;
    }
  }
  if (!call || !value || fx->nchain == 0) return 0;
  fx->call = call; fx->source = value; fx->carrier = values[arg];
  ins = ir_load_acq(&fx->ir[fx->helper]);
  if (ins.op1 < REF_FIRST || ins.op1 >= fx->helper) return 0;
  {
    IRIns tail = ir_load_acq(&fx->ir[ins.op1]);
    IRIns head;
    if (tail.o != IR_CARG || tail.t.irt != IRT_NIL || tail.op1 < REF_FIRST) return 0;
    head = ir_load_acq(&fx->ir[tail.op1]);
    if (head.o != IR_CARG || head.op1 != tail.op2 || head.op2 < REF_FIRST) return 0;
    fx->root1 = tail.op2; fx->root2 = head.op2;
    if (ir_load_acq(&fx->ir[fx->root1]).o != IR_TMPREF ||
        ir_load_acq(&fx->ir[fx->root2]).o != IR_TMPREF) return 0;
  }
  /* Find the CALLXS input snapshot, not the rooted read's retry snapshot. */
  for (r = fx->call; r > REF_FIRST;) {
    IRRef p = --r;
    if (ir_load_acq(&fx->ir[p]).o == IR_XSAVE) { fx->xsave = p; break; }
  }
  if (!fx->xsave || !snap_for_ref(fx, fx->xsave, &fx->xsnap)) return 0;
  {
    IRIns mark = ir_load_acq(&fx->ir[fx->xsave]);
    uint32_t off = (uint32_t)mark.op1 | ((uint32_t)mark.op2 << 15);
    const BCIns *bc;
    BCIns src;
    if (mark.o != IR_XSAVE || !off || off >= fx->pt->sizebc) return 0;
    bc = &proto_bc(fx->pt)[off-1u]; src = loadbc(bc);
    if (bc_op(src) != BC_CALL || !bc_c(src) ||
        (unsigned)bc_c(src)-1u != count) return 0;
    fx->arg_slot = (BCReg)(bc_a(src)+2u*(1u+LJ_FR2)+1u+fx->spec.arg);
    {
      IRRef staged;
      if (!snapshot_slot(fx, fx->xsnap, fx->arg_slot, &fx->arg_entry, &staged) ||
          staged != fx->source ||
          (fx->snapmap[fx->arg_entry] & SNAP_NORESTORE)) return 0;
    }
    return 1;
  }
}

static void reset_fixture(Fixture *fx)
{
  IRRef nk = trace_nk_acq(fx->owner), full = trace_nins_acq(fx->owner);
  memset(fx->ir, 0, (size_t)full*sizeof(IRIns));
  memcpy(&fx->ir[nk], &trace_ir_acq(fx->owner)[nk], (size_t)(full-nk)*sizeof(IRIns));
  memcpy(fx->snap, trace_snap_acq(fx->owner), (size_t)fx->nsnap*sizeof(SnapShot));
  memcpy(fx->snapmap, trace_snapmap_acq(fx->owner), (size_t)fx->nsnapmap*sizeof(SnapEntry));
  fx->view_owner = *fx->owner; fx->view_owner.ir=fx->ir;
  fx->view_owner.snap=fx->snap; fx->view_owner.snapmap=fx->snapmap;
  fx->nins=semantic_nins(fx->owner);
  assert(locate_call(fx));
}

static JSave enter_semantic(Fixture *fx)
{
  jit_State *J=fx->J;
  JSave s={J->curfinal,J->pt,J->startpc,J->loopref,J->parent,J->exitno,
           J->baseslot,J->framedepth,J->retdepth,J->ktrace};
  fx->semantic=fx->view_owner; fx->semantic.nins=fx->nins;
  fx->ir[fx->ktrace].o=IR_KNUM; fx->ir[fx->ktrace].t.irt=IRT_P64;
  fx->ir[fx->ktrace].op12=0; fx->ir[fx->ktrace].prev=0;
  fx->ir[fx->ktrace+1u].tv.u64=0;
  J->curfinal=NULL; J->pt=fx->pt; J->startpc=trace_startpc_acq(fx->owner);
  J->loopref=0;
  for(IRRef r=REF_FIRST;r<fx->nins;r++) if(ir_load_acq(&fx->ir[r]).o==IR_LOOP){J->loopref=r;break;}
  assert(J->loopref); J->parent=0; J->exitno=0; J->baseslot=1u+LJ_FR2;
  J->framedepth=0; J->retdepth=0; J->ktrace=fx->ktrace;
  return s;
}

static void leave_semantic(jit_State *J, JSave *s)
{
  J->curfinal=s->curfinal; J->pt=s->pt; J->startpc=s->startpc;
  J->loopref=s->loopref; J->parent=s->parent; J->exitno=s->exitno;
  J->baseslot=s->baseslot; J->framedepth=s->framedepth;
  J->retdepth=s->retdepth; J->ktrace=s->ktrace;
}

static int check_semantic(Fixture *fx)
{
  uint32_t *pc=(uint32_t *)trace_startpc_acq(fx->owner), old=la_load32_acq(pc);
  JSave s; int ok;
  la_store32_rel(pc,fx->owner->startins); s=enter_semantic(fx);
  ok=lj_asm_arm64_ir_admit(fx->J,&fx->semantic,NULL);
  leave_semantic(fx->J,&s); la_store32_rel(pc,old); return ok;
}

static LJArm64PostRAView postra_view(Fixture *fx)
{
  LJArm64PostRAView v={0};
  v.ir=fx->ir; v.owner=&fx->view_owner; v.cts=fx->cts;
  v.snap=fx->snap; v.snapmap=fx->snapmap;
  v.proto_bc=proto_bc(fx->pt); v.proto=fx->pt;
  v.startpc=trace_startpc_acq(fx->owner); v.nins=trace_nins_acq(&fx->view_owner);
  v.nk=trace_nk_acq(fx->owner); v.nsnap=fx->nsnap; v.nsnapmap=fx->nsnapmap;
  v.spadjust=trace_spadjust_acq(fx->owner); v.proto_sizebc=fx->pt->sizebc;
  v.proto_numparams=fx->pt->numparams; v.root_topslot=trace_topslot_acq(fx->owner);
  v.startins=trace_startins_acq(fx->owner); return v;
}

static void mutate(Fixture *fx, enum Mutation m)
{
  IRRef first=fx->chain[0], last=fx->chain[fx->nchain-1u], alt=0;
  switch(m) {
  case M_NUM_SOURCE:
    for(IRRef r=REF_FIRST;r<fx->call;r++) if(r!=fx->source &&
      ir_load_acq(&fx->ir[r]).o==IR_VLOAD && irt_type(ir_load_acq(&fx->ir[r]).t)==IRT_NUM){alt=r;break;}
    if(!alt) for(IRRef r=trace_nk_acq(fx->owner);r<REF_TRUE;r++){
      IRIns k=ir_load_acq(&fx->ir[r]); if(k.o==IR_KNUM && k.t.irt==IRT_NUM){alt=r;break;}
      if(k.o==IR_KNUM||k.o==IR_KINT64||(LJ_GC64&&(k.o==IR_KGC||k.o==IR_KPTR||k.o==IR_KKPTR)))r++;
    }
    assert(alt); fx->ir[first].op1=alt; break;
  case M_NUM_MODE: fx->ir[first].op2 ^= IRCONV_CHECK; break;
  case M_NUM_TYPE:
    fx->ir[first].t.irt=(fx->spec.width==8 && fx->spec.is_unsigned) ?
                         IRT_I64 : IRT_U64;
    break;
  case M_FINAL_TYPE: fx->ir[last].t.irt=IRT_NUM; break;
  case M_NARROW_MODE:
    assert(fx->spec.width<4 && fx->nchain>=3);
    fx->ir[last].op2 ^= IRCONV_SEXT; break;
  case M_ABI_SIGN: {
    const char *other = fx->spec.is_unsigned ? "__numeric_i" : "__numeric_u";
    char name[32];
    IRRef ctype = 0;
    (void)snprintf(name,sizeof(name),"%s%u",other,fx->spec.width*8u);
    for (IRRef r=REF_FIRST;r<fx->call;r++) {
      IRIns fld=ir_load_acq(&fx->ir[r]);
      if (fld.o != IR_FLOAD || fld.op2 != IRFL_CDATA_CTYPEID) continue;
      for (IRRef q=r+1u;q<fx->call;q++) {
        IRIns eq=ir_load_acq(&fx->ir[q]); IRRef k;
        if (eq.o!=IR_EQ || !irt_isguard(eq.t) ||
            (eq.op1!=r && eq.op2!=r)) continue;
        k=eq.op1==r?eq.op2:eq.op1;
        if (k<trace_nk_acq(fx->owner) || ir_load_acq(&fx->ir[k]).o!=IR_KINT) continue;
        ctype=k; break;
      }
      if (ctype) break;
    }
    assert(ctype); fx->ir[ctype].i=(int32_t)global_ctype(fx->L,name);
    break;
  }
  case M_XSAVE_SOURCE: {
    MSize ofs=snap_mapofs_acq(&fx->snap[fx->xsnap]), i;
    int changed=0;
    for(i=0;i<snap_nent_acq(&fx->snap[fx->xsnap]);i++)
      if(snap_slot(fx->snapmap[ofs+i])==fx->arg_slot){
        for(IRRef r=trace_nk_acq(fx->owner);r<REF_TRUE;r++){
          IRIns k=ir_load_acq(&fx->ir[r]); if(k.o==IR_KNUM&&k.t.irt==IRT_NUM){
            fx->snapmap[ofs+i]=snap_setref(fx->snapmap[ofs+i],r);changed=1;break;}
          if(k.o==IR_KNUM||k.o==IR_KINT64||(LJ_GC64&&(k.o==IR_KGC||k.o==IR_KPTR||k.o==IR_KKPTR)))r++;
        }
        break;
      }
    assert(changed); break;
  }
  case M_XSAVE_INTERMEDIATE: {
    MSize ofs=snap_mapofs_acq(&fx->snap[fx->xsnap]), i;
    int changed=0;
    for(i=0;i<snap_nent_acq(&fx->snap[fx->xsnap]);i++)
      if(snap_slot(fx->snapmap[ofs+i])==fx->arg_slot){
        fx->snapmap[ofs+i]=snap_setref(fx->snapmap[ofs+i],first);changed=1;break;
      }
    assert(changed); break;
  }
  case M_XSAVE_NORESTORE: {
    MSize ofs=snap_mapofs_acq(&fx->snap[fx->xsnap]), i;
    for(i=0;i<snap_nent_acq(&fx->snap[fx->xsnap]);i++)
      if(snap_slot(fx->snapmap[ofs+i])==fx->arg_slot){fx->snapmap[ofs+i]|=SNAP_NORESTORE;break;}
    assert(i<snap_nent_acq(&fx->snap[fx->xsnap])); break;
  }
  case M_XSAVE_TRUNCATED: fx->snap[fx->xsnap].nslots=fx->arg_slot; break;
  case M_STACK_WRONG_CARRIER: {
    IRRef nodes[32], carg=ir_load_acq(&fx->ir[fx->call]).op1;
    IRRef vals[32]; unsigned n=0, nvals=carg_values(fx,fx->call,vals,31);
    while(carg>=REF_FIRST && carg<fx->call && fx->ir[carg].o==IR_CARG) {
      assert(n<32); nodes[n++]=carg; carg=fx->ir[carg].op1;
    }
    assert(fx->spec.arg>0 && n>=fx->spec.arg && nvals>0);
    carg=nodes[n-fx->spec.arg];
    assert(fx->ir[carg].o==IR_CARG);
    fx->ir[carg].op2=vals[0];
    break;
  }
  default: assert(!"post-RA-only mutation");
  }
}

static void expect_both(Fixture *fx, enum Mutation m, const char *label)
{
  int ok; reset_fixture(fx); mutate(fx,m); ok=check_semantic(fx);
  if(ok) fprintf(stderr,"numeric semantic accepted %s\n",label);
  assert(!ok); semantic_rejects++;
  reset_fixture(fx); mutate(fx,m); { LJArm64PostRAView v=postra_view(fx); ok=lj_asm_arm64_postra_admit(&v,NULL); }
  if(ok) fprintf(stderr,"numeric post-RA accepted %s\n",label);
  assert(!ok); postra_rejects++;
}

static void expect_postra(Fixture *fx, enum Mutation m, const char *label)
{
  LJArm64PostRAView v; IRIns *ins;
  reset_fixture(fx); ins=&fx->ir[fx->carrier];
  if(m==M_BAD_BANK) { ins->r=RID_D0; ins->s=SPS_NONE; }
  else if(m==M_BAD_SPILL_ALIGN) { ins->r=RID_NONE; ins->s=(uint8_t)(SPS_FIRST+1u); }
  else if(m==M_BAD_SPILL_EXTENT) {
    MSize cap=SPS_FIXED+trace_spadjust_acq(fx->owner)/sizeof(int32_t);
    assert(cap<UINT8_MAX); ins->r=RID_NONE; ins->s=(uint8_t)cap;
  } else assert(0);
  v=postra_view(fx);
  if(lj_asm_arm64_postra_admit(&v,NULL)) fprintf(stderr,"numeric post-RA accepted %s\n",label);
  assert(!lj_asm_arm64_postra_admit(&v,NULL)); postra_rejects++;
}

static void capture(lua_State *L, jit_State *J, Fixture *fx, NumCase spec)
{
  fx->L=L; fx->J=J; fx->spec=spec; fx->pt=global_proto(L,spec.runner);
  fx->owner=find_trace(J,fx->pt); assert(fx->owner);
  fx->cts=ctype_ctsG(G(L)); fx->ktrace=find_ktrace(fx->owner);
  fx->nsnap=trace_nsnap_acq(fx->owner); fx->nsnapmap=trace_nsnapmap_acq(fx->owner);
  assert(fx->cts&&fx->ktrace&&fx->nsnap&&fx->nsnapmap);
  fx->ir=(IRIns *)calloc(trace_nins_acq(fx->owner),sizeof(IRIns));
  fx->snap=(SnapShot *)calloc(fx->nsnap,sizeof(SnapShot));
  fx->snapmap=(SnapEntry *)calloc(fx->nsnapmap,sizeof(SnapEntry));
  assert(fx->ir&&fx->snap&&fx->snapmap); reset_fixture(fx);
}

static void free_fixture(Fixture *fx)
{ free(fx->snapmap); free(fx->snap); free(fx->ir); }

static void run_case(lua_State *L, const NumCase *c)
{
  lua_getglobal(L,"__numeric_configure"); lua_pushinteger(L,c->kind);
  assert(lua_pcall(L,1,0,0)==LUA_OK);
  lua_getglobal(L,"__numeric_driver"); lua_getglobal(L,c->runner);
  lua_getglobal(L,"__numeric_table"); lua_getglobal(L,c->callee); lua_pushinteger(L,RUNS);
  assert(lua_pcall(L,4,1,0)==LUA_OK && lua_isnumber(L,-1));
  assert(lua_tonumber(L,-1)==RUNS*1.25); lua_pop(L,1);
}

static void test_case(lua_State *L, jit_State *J, const NumCase *spec)
{
  Fixture fx={0}; JSave s; uint32_t *pc; uint32_t saved;
  run_case(L,spec); capture(L,J,&fx,*spec);
  pc=(uint32_t *)trace_startpc_acq(fx.owner); saved=la_load32_acq(pc);
  la_store32_rel(pc,fx.owner->startins); s=enter_semantic(&fx);
  assert(lj_asm_arm64_ir_admit(J,&fx.semantic,NULL)); leave_semantic(J,&s);
  la_store32_rel(pc,saved);
  reset_fixture(&fx);
  { LJArm64PostRAView v=postra_view(&fx); assert(lj_asm_arm64_postra_admit(&v,NULL)); }
  positives+=2;
  expect_both(&fx,M_NUM_SOURCE,"NUM conversion source identity");
  expect_both(&fx,M_NUM_MODE,"NUM to integer must use ANY");
  expect_both(&fx,M_NUM_TYPE,"first carrier width/type");
  expect_both(&fx,M_FINAL_TYPE,"final ABI carrier type");
  if(spec->arg==0) expect_both(&fx,M_ABI_SIGN,"function ABI signedness mismatch");
  expect_both(&fx,M_XSAVE_SOURCE,"XSAVE must retain original Lua NUM");
  expect_both(&fx,M_XSAVE_INTERMEDIATE,"XSAVE cannot expose converted carrier");
  expect_both(&fx,M_XSAVE_NORESTORE,"CALLXS input must be restorable");
  expect_both(&fx,M_XSAVE_TRUNCATED,"CALLXS source argument slot is present");
  if(spec->width<4) expect_both(&fx,M_NARROW_MODE,"narrow signedness extension");
  if(spec->arg>0) expect_both(&fx,M_STACK_WRONG_CARRIER,"stack CARG argument order");
  expect_postra(&fx,M_BAD_BANK,"numeric carrier uses wrong register bank");
  if(spec->width>=8) expect_postra(&fx,M_BAD_SPILL_ALIGN,"64-bit carrier spill alignment");
  expect_postra(&fx,M_BAD_SPILL_EXTENT,"carrier spill outside frame");
  free_fixture(&fx);
}

int main(void)
{
  static const NumCase cases[CASES]={
    {"__numeric_run_i8","__numeric_i8",0,0,1,0},
    {"__numeric_run_u8","__numeric_u8",0,1,1,1},
    {"__numeric_run_i16","__numeric_i16",0,2,2,0},
    {"__numeric_run_u16","__numeric_u16",0,3,2,1},
    {"__numeric_run_i32","__numeric_i32",0,4,4,0},
    {"__numeric_run_u32","__numeric_u32",0,5,4,1},
    {"__numeric_run_i64","__numeric_i64",0,6,8,0},
    {"__numeric_run_u64","__numeric_u64",0,7,8,1},
    {"__numeric_run_stack","__numeric_stack",8,8,1,0}
  };
  lua_State *L=ljt_lua_newstate_openlibs(); jit_State *J=G2J(G(L));
  const char *so=getenv("LJ_M7_FFI_NUMERIC_ARGS_SO"); assert(so&&*so);
  ljt_lua_dostring(L,
    "local ffi=require('ffi'); local threading=require('threading')\n"
    "ffi.cdef[[void lj_ffi_numeric_configure(int32_t); double lj_ffi_numeric_i8(int8_t); double lj_ffi_numeric_u8(uint8_t); double lj_ffi_numeric_i16(int16_t); double lj_ffi_numeric_u16(uint16_t); double lj_ffi_numeric_i32(int32_t); double lj_ffi_numeric_u32(uint32_t); double lj_ffi_numeric_i64(int64_t); double lj_ffi_numeric_u64(uint64_t); double lj_ffi_numeric_stack(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int8_t,uint8_t,int16_t,uint16_t,int32_t,uint32_t,int64_t,uint64_t);]]\n"
    "local lib=ffi.load(assert(os.getenv('LJ_M7_FFI_NUMERIC_ARGS_SO')))\n"
    "_G.__numeric_i8=lib.lj_ffi_numeric_i8; _G.__numeric_u8=lib.lj_ffi_numeric_u8; _G.__numeric_i16=lib.lj_ffi_numeric_i16; _G.__numeric_u16=lib.lj_ffi_numeric_u16; _G.__numeric_i32=lib.lj_ffi_numeric_i32; _G.__numeric_u32=lib.lj_ffi_numeric_u32; _G.__numeric_i64=lib.lj_ffi_numeric_i64; _G.__numeric_u64=lib.lj_ffi_numeric_u64; _G.__numeric_stack=lib.lj_ffi_numeric_stack\n"
    "function _G.__numeric_configure(k) lib.lj_ffi_numeric_configure(k) end; _G.__numeric_table={value=-3.75}\n"
    "function _G.__numeric_run_i8(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end; function _G.__numeric_run_u8(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i16(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end; function _G.__numeric_run_u16(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i32(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end; function _G.__numeric_run_u32(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_i64(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end; function _G.__numeric_run_u64(t,f,n) local s=.5; for i=1,n do s=s+f(t.value) end; return s-.5 end\n"
    "function _G.__numeric_run_stack(t,f,n) local s=.5; for i=1,n do s=s+f(101,102,103,104,105,106,107,108,t.value,t.value,t.value,t.value,t.value,t.value,t.value,t.value) end; return s-.5 end\n"
    "function _G.__numeric_driver(run,t,f,n) local r=run(t,f,n); return r end\n"
    "_G.__numeric_ready=threading.channel(1); _G.__numeric_stop=threading.channel(1); _G.__numeric_peer=assert(threading.spawn(function(ready,stop) assert(ready:send(true,5)); assert(select(2,stop:recv(120))); return true end,__numeric_ready,__numeric_stop)); assert(select(2,__numeric_ready:recv(5)))\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=128')\n");
  for(unsigned i=0;i<CASES;i++) test_case(L,J,&cases[i]);
  ljt_lua_dostring(L,"assert(__numeric_stop:send(true,5)); local ok,v=__numeric_peer:join(5); assert(ok and v)");
  lua_close(L);
  printf("t-arm64-jit-ffi-numeric-args-admission OK: %u original admissions, %u semantic and %u post-RA negatives\n",positives,semantic_rejects,postra_rejects);
  return 0;
}

#else
int main(void)
{
  puts("t-arm64-jit-ffi-numeric-args-admission SKIP: requires experimental Darwin ARM64");
  return 0;
}
#endif
