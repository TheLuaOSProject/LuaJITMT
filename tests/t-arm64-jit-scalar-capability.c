/*
** ARM64 scalar LOOP capability: execute varied scalar dataflow, then verify
** independent recorder and immutable allocation certificates. The immutable
** IR copy ends at a guard page so malformed forward references cannot hide an
** out-of-bounds read. Semantic mutations use a separate copy and temporarily
** restore only this single-threaded test state's original LOOP bytecode.
*/
#include <stdio.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) &&     \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) &&                                     \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) &&                              \
    defined(LJ_TRACE_TEST_HELPERS)

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_gc2.h"
#include "lj_ctype.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

static int scalar_tostr_kind(const char *kind)
{
  return kind != NULL &&
    (strncmp(kind, "numeric_tostr", 13) == 0 ||
     strncmp(kind, "integer_tostr", 13) == 0);
}

static int scalar_integer_tostr_kind(const char *kind)
{
  return kind != NULL && strncmp(kind, "integer_tostr", 13) == 0;
}

static int modulo_admits(jit_State *J, GCtrace *T,
                          const LJArm64PostRAView *v)
{
  LJArm64IRReject reject;
  return J ? lj_asm_arm64_ir_admit(J, T, &reject) :
             lj_asm_arm64_postra_admit(v, NULL);
}

/* Mutate one private trace copy; each pass must reject on its own. */
static void mutate_modulo(jit_State *J, GCtrace *T, LJArm64PostRAView *v,
                           IRRef n, IRRef loop)
{
  IRIns *copy = J ? T->ir : (IRIns *)(void *)v->ir;
  IRRef r, mod = 0, zero = 0, number = 0, guard = 0;
  IRIns saved;
  for (r = REF_FIRST; r < n; r++)
    if (copy[r].o == IR_MOD) { mod = r; break; }
  for (r = v->nk; r < REF_TRUE; r++) {
    IRIns k = copy[r];
    if (k.o == IR_KINT && k.i == 0) zero = r;
    if (k.o == IR_KNUM) number = r;
    if (k.o == IR_KNUM || k.o == IR_KGC || k.o == IR_KKPTR) r++;
  }
  for (r = REF_FIRST; r < mod && !number; r++)
    if (irt_type(copy[r].t) == IRT_NUM) number = r;
  assert(mod && number && (copy[mod].t.irt & ~IRT_ISPHI) == IRT_INT &&
         modulo_admits(J, T, v));
  saved = copy[mod];
  copy[mod].t.irt = IRT_NUM;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  copy[mod].t.irt = IRT_INT|IRT_GUARD;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  copy[mod].op1 = number;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  copy[mod].op2 = number;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  copy[mod].op1 = n;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  copy[mod].op2 = n;
  assert(!modulo_admits(J, T, v));
  copy[mod] = saved;
  if (irref_isk(saved.op2)) {
    IRIns divisor = copy[saved.op2];
    assert(divisor.o == IR_KINT && divisor.i != 0);
    copy[saved.op2].i = 0;
    assert(!modulo_admits(J, T, v));
    copy[saved.op2] = divisor;
  } else {
    assert(zero);
    copy[mod].op2 = zero;
    assert(!modulo_admits(J, T, v));
    copy[mod] = saved;
    for (r = saved.op2+1u; r < mod; r++)
      if (copy[r].o == IR_NE && copy[r].t.irt == (IRT_INT|IRT_GUARD) &&
          copy[r].op1 == saved.op2 && copy[r].op2 == zero) {
        assert(!guard); guard = r;
      }
    assert(guard);
    IRIns saved_guard = copy[guard];
    copy[guard].o = IR_EQ;
    assert(!modulo_admits(J, T, v));
    copy[guard] = saved_guard;
    copy[guard].op1 = saved.op1;
    assert(saved.op1 != saved.op2 && !modulo_admits(J, T, v));
    copy[guard] = saved_guard;
    copy[guard].t.irt = IRT_INT;
    assert(!modulo_admits(J, T, v));
    copy[guard] = saved_guard;
    IRIns zero_value = copy[zero];
    copy[zero].i = 1;
    assert(!modulo_admits(J, T, v));
    copy[zero] = zero_value;
    if (!J) {
      IRIns divisor = copy[saved.op2];
      assert(lj_asm_arm64_test_scalar_math_live_across(v, n, loop, mod));
      copy[saved.op2].prev = REGSP(RID_X0, SPS_NONE);
      assert(!lj_asm_arm64_test_scalar_math_live_across(v, n, loop, mod));
      assert(!modulo_admits(J, T, v));
      copy[saved.op2] = divisor;
    }
  }
  assert(modulo_admits(J, T, v));
  printf("MOD %s: INT operands, nonzero divisor and call survival; malformed proofs rejected dynamic=%d\n",
         J ? "semantic" : "postRA", !irref_isk(saved.op2));
}

static void mutate_modulo_phi(LJArm64PostRAView *v, IRRef n, IRRef loop)
{
  IRIns *copy = (IRIns *)(void *)v->ir;
  IRRef ref, source=0, call=0, phi=0, rename=0;
  MSize snap, loopsnap=0, count=0;
  IRIns saved;
  for (ref=loop+1u; ref<n; ref++) {
    IRIns q=copy[ref];
    if (q.o==IR_PHI && q.t.irt==IRT_INT && q.op1<loop &&
        q.op1>=REF_FIRST && q.op2>loop && q.op2<ref &&
        copy[q.op1].o==IR_MOD && copy[q.op2].o==IR_MOD) {
      assert(!phi);phi=ref;source=q.op1;call=q.op2;
    }
  }
  assert(phi && source && call);
  for (snap=0;snap<v->nsnap;snap++)
    if (snap_ref_acq(&v->snap[snap])==loop) {loopsnap=snap;count++;}
  assert(count==1);
  for (ref=n;ref<v->nins;ref++)
    if (copy[ref].o==IR_RENAME && copy[ref].op1==source &&
        copy[ref].op2==loopsnap) {assert(!rename);rename=ref;}
  assert(rename && copy[rename].r>=RID_X19 && copy[rename].r<=RID_X28);
  assert(lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  saved=copy[rename];
  copy[rename].prev=REGSP(RID_X0,SPS_NONE);
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[rename]=saved;
  copy[rename].op2=loopsnap==0?1:0;
  assert(copy[rename].op2<v->nsnap);
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[rename]=saved;
  copy[rename].op1=copy[call].op1;
  assert(copy[rename].op1!=source);
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  copy[rename]=saved;
  saved=copy[source];
  copy[source].o=IR_ADD;
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[source]=saved;
  saved=copy[call];
  copy[call].op1=source;  /* Introduce a real body use outside the PHI. */
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[call]=saved;
  copy[call].t.irt=IRT_NUM;
  assert(!lj_asm_arm64_test_scalar_math_live_across(v,n,loop,call));
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[call]=saved;
  saved=copy[phi];
  copy[phi].op1=call;
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[phi]=saved;
  copy[phi].op2=source;
  assert(!lj_asm_arm64_postra_admit(v,NULL));
  copy[phi]=saved;
  assert(lj_asm_arm64_postra_admit(v,NULL));
  printf("MOD loop result=%u body=%u PHI=%u snapshot=%u rename=%u: exact preserved-location and extra-use mutations rejected\n",
         source,call,phi,(unsigned)loopsnap,rename);
}

static void mutate_layout(lua_State *L, GCtrace *T, const char *mathkind)
{
  LJArm64PostRAView v = {0};
  size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
  size_t irbytes = T->nins * sizeof(IRIns),
         mapsize = (irbytes + pagesize - 1) & ~(pagesize - 1);
  void *guarded = mmap(NULL, mapsize + pagesize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  IRIns *copy;
  assert(guarded != MAP_FAILED &&
         mprotect((char *)guarded + mapsize, pagesize, PROT_NONE) == 0);
  copy = (IRIns *)((char *)guarded + mapsize - irbytes);
  SnapEntry *map = malloc(T->nsnapmap * sizeof(*map));
  SnapShot *snaps = malloc(T->nsnap * sizeof(*snaps));
  IRRef ref, loop = 0, num = 0, knum = 0, fpmath = 0, mathop = 0;
  IRRef tostr = 0, intref = 0, lref = 0, tmpreset = 0;
  IRRef maskload = 0, mathcall = 0;
  IRRef mathguard = 0, n;
  int math_builtin = mathkind && strncmp(mathkind, "math_", 5) == 0;
  int math_passthrough = mathkind &&
    (strcmp(mathkind, "math_min_one") == 0 ||
     strcmp(mathkind, "math_max_one") == 0);
  int math_proof = math_builtin ||
                   (mathkind && strcmp(mathkind, "numeric_neg") == 0);
  int tostr_mode = scalar_integer_tostr_kind(mathkind) ?
    IRTOSTR_INT : IRTOSTR_NUM;
  IRType tostr_input_type = scalar_integer_tostr_kind(mathkind) ?
    IRT_INT : IRT_NUM;
  GCproto *pt = trace_startpt_acq(T);
  assert(copy && map && snaps);
  memcpy(copy + T->nk, T->ir + T->nk, (T->nins - T->nk) * sizeof(*copy));
  memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
  memcpy(snaps, T->snap, T->nsnap * sizeof(*snaps));
  v.ir = copy;
  v.owner = T;
  v.cts = ctype_ctsG(G(L));
  v.snap = snaps;
  v.snapmap = map;
  v.proto_bc = proto_bc(pt);
  v.nins = T->nins;
  v.nk = T->nk;
  v.nsnap = T->nsnap;
  v.nsnapmap = T->nsnapmap;
  v.spadjust = T->spadjust;
  v.proto_sizebc = pt->sizebc;
  v.proto_numparams = pt->numparams;
  v.root_topslot = T->topslot;
  v.startins = T->startins;
  v.proto = pt;
  v.startpc = trace_startpc_acq(T);
  v.base_delta = 0;
  assert(lj_asm_arm64_postra_admit(&v, &n));
  for (ref = REF_FIRST; ref < n; ref++) {
    if (copy[ref].o == IR_LOOP)
      loop = ref;
    if (copy[ref].o == IR_FPMATH)
      fpmath = ref;
    if (copy[ref].o == IR_CALLN)
      mathcall = ref;
    if (copy[ref].o == IR_TOSTR && !tostr)
      tostr = ref;
    if (copy[ref].o == IR_LREF && !lref)
      lref = ref;
    if (copy[ref].o == IR_CALLS &&
        copy[ref].op2 == IRCALL_lj_buf_tmp_reset && !tmpreset)
      tmpreset = ref;
    if (!intref && copy[ref].o == IR_SLOAD &&
        irt_type(copy[ref].t) == IRT_INT)
      intref = ref;
    if ((copy[ref].o == IR_ABS || copy[ref].o == IR_MIN ||
         copy[ref].o == IR_MAX || copy[ref].o == IR_POW ||
         copy[ref].o == IR_LDEXP ||
         (copy[ref].o == IR_NEG && irt_type(copy[ref].t) == IRT_NUM)) &&
        !mathop)
      mathop = ref;
    if (copy[ref].o == IR_EQ && irt_type(copy[ref].t) == IRT_FUNC &&
        copy[ref].op2 < REF_TRUE && copy[copy[ref].op2].o == IR_KGC)
      mathguard = ref;
    if ((copy[ref].o == IR_ADD || copy[ref].o == IR_SUB) &&
        irt_isnum(copy[ref].t))
      num = ref;
    if (copy[ref].o == IR_FLOAD && copy[ref].t.irt == IRT_NUM &&
        copy[ref].op1 == REF_NIL &&
        (copy[ref].op2 == offsetof(GG_State, g.ksimd)/4u ||
         copy[ref].op2 == (offsetof(GG_State, g.ksimd) + 16u)/4u))
      maskload = ref;
  }
  for (ref = T->nk; ref < REF_TRUE; ref++)
    if (copy[ref].o == IR_KNUM) {
      knum = ref;
      break;
    }
  assert(loop);
  if (mathkind && strncmp(mathkind, "integer_mod", 11) == 0)
    mutate_modulo(NULL, T, &v, n, loop);
  if (mathkind && strncmp(mathkind, "integer_mod_result", 18) == 0)
    mutate_modulo_phi(&v, n, loop);
  if (math_proof) assert((fpmath || mathcall || mathop || math_passthrough) &&
                         (!math_builtin || mathguard));
  if (mathkind && strcmp(mathkind, "math_helpers_snapshot_pair") == 0) {
    IRRef call, value, chosen_call = 0, chosen_value = 0, chosen_rename = 0;
    SnapNo chosen_snap = 0;
    MSize s, i;
    /* Select from actual SSA and snapshot facts: a NUM value remains in a
       later snapshot across a helper call, and that snapshot uses a matching
       allocated rename. */
    for (call = loop+1u; call < n && !chosen_value; call++) {
      if (copy[call].o != IR_CALLN &&
          !(copy[call].o == IR_FPMATH && copy[call].op2 >= IRFPM_LOG))
        continue;
      for (value = REF_FIRST; value < call && !chosen_value; value++) {
        int in_snapshot = 0;
        if (irt_type(copy[value].t) != IRT_NUM) continue;
        for (s = 0; s < T->nsnap && !in_snapshot; s++) {
          IRRef at = snap_ref_acq(&T->snap[s]);
          MSize ofs = snap_mapofs_acq(&T->snap[s]);
          MSize nent = snap_nent_acq(&T->snap[s]);
          if (at <= call) continue;
          for (i = 0; i < nent; i++)
            if (snap_ref(snapentry_acq(&map[ofs+i])) == value) {
              in_snapshot = 1;
              chosen_snap = (SnapNo)s;
              break;
            }
        }
        if (!in_snapshot) continue;
        for (ref = n; ref < T->nins; ref++) {
          if (copy[ref].o == IR_RENAME && copy[ref].op1 == value &&
              copy[ref].op2 <= chosen_snap) {
            chosen_call = call;
            chosen_value = value;
            chosen_rename = ref;
            break;
          }
        }
      }
    }
    assert(chosen_call && chosen_value < chosen_call && chosen_rename &&
           (copy[chosen_call].o == IR_CALLN ||
            (copy[chosen_call].o == IR_FPMATH &&
             copy[chosen_call].op2 >= IRFPM_LOG)) &&
           irt_type(copy[chosen_value].t) == IRT_NUM &&
           snap_ref_acq(&T->snap[chosen_snap]) > chosen_call &&
           copy[chosen_rename].op1 == chosen_value &&
           copy[chosen_rename].prev == T->ir[chosen_rename].prev);
    {
      int carried = 0;
      for (ref = loop+1u; ref < n; ref++)
        carried |= copy[ref].o == IR_PHI &&
          irt_type(copy[ref].t) == IRT_NUM &&
          (copy[ref].op1 == chosen_value || copy[ref].op2 == chosen_value);
      assert(carried);
    }
    {
      IRIns saved_value = copy[chosen_value];
      IRIns saved_rename = copy[chosen_rename];
      RegSP volatile_location = REGSP(RID_D0, SPS_NONE);
      assert(lj_asm_arm64_test_scalar_math_live_across(
          &v, n, loop, chosen_call));
      assert(ra_hasreg(regsp_reg(volatile_location)) &&
             regsp_reg(volatile_location) == RID_D0 &&
             !ra_hasspill(regsp_spill(volatile_location)));
      /* The snapshot's renamed location must independently survive the
         helper call even when the value's base allocation is preserved. */
      copy[chosen_rename].prev = volatile_location;
      assert(!lj_asm_arm64_test_scalar_math_live_across(
          &v, n, loop, chosen_call));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[chosen_rename] = saved_rename;
      assert(memcmp(&copy[chosen_value], &saved_value,
                    sizeof(saved_value)) == 0);
      assert(lj_asm_arm64_postra_admit(&v, NULL));
      printf("MATH loop call=%u PHI-input=%u type=%u snapshot=%u rename=%u call-survival mutation rejected\n",
             (unsigned)chosen_call, (unsigned)chosen_value,
             (unsigned)irt_type(copy[chosen_value].t),
             (unsigned)chosen_snap, (unsigned)chosen_rename);
    }
  }
  if (scalar_tostr_kind(mathkind)) {
    IRIns saved, saved_lref, saved_reset;
    IRRef source_tostr = 0, body_tostr = 0, tostr_phi = 0;
    MSize mapidx;
    MSize badsnap;
    SnapEntry saved_map;
    SnapShot saved_snap;
    assert(tostr && intref && tostr < n &&
           (copy[tostr].t.irt & ~IRT_ISPHI) == IRT_STR &&
           copy[tostr].op2 == tostr_mode && copy[tostr].op1 < tostr &&
           irt_type(copy[copy[tostr].op1].t) == tostr_input_type);
    saved = copy[tostr];
    copy[tostr].op2 = tostr_mode == IRTOSTR_INT ?
      IRTOSTR_NUM : IRTOSTR_INT;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tostr] = saved;
    copy[tostr].op2 = IRTOSTR_CHAR;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tostr] = saved;
    copy[tostr].op1 = tostr_mode == IRTOSTR_INT ? REF_NIL : intref;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tostr] = saved;
    copy[tostr].op1 = n;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tostr] = saved;
    assert(lref && tmpreset && copy[lref].t.irt == IRT_THREAD &&
           copy[tmpreset].t.irt == IRT_PGC && copy[tmpreset].op1 == lref);
    saved_lref = copy[lref];
    saved_reset = copy[tmpreset];
    copy[tmpreset].op2 = IRCALL_lj_buf_tmp_reset + 1u;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tmpreset] = saved_reset;
    copy[tmpreset].op1 = intref;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tmpreset] = saved_reset;
    copy[tmpreset].t.irt = IRT_INT;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[tmpreset] = saved_reset;
    copy[lref].op1 = 1;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[lref] = saved_lref;
    copy[lref].t.irt = IRT_PTR;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[lref] = saved_lref;
    for (mapidx = 0; mapidx < T->nsnapmap; mapidx++)
      if (snap_ref(map[mapidx]) == tostr) break;
    assert(mapidx < T->nsnapmap);
    saved_map = map[mapidx];
    map[mapidx] = snap_setref(saved_map, tmpreset);
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    map[mapidx] = snap_setref(saved_map, lref);
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    map[mapidx] = saved_map;
    for (badsnap = 0; badsnap < T->nsnap; badsnap++)
      if (snap_nent_acq(&snaps[badsnap]) != 0) break;
    assert(badsnap < T->nsnap);
    saved_snap = snaps[badsnap];
    snaps[badsnap].mapofs = (uint32_t)T->nsnapmap;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    snaps[badsnap] = saved_snap;
    assert(lj_asm_arm64_postra_admit(&v, NULL));
    for (ref = REF_FIRST; ref < loop; ref++)
      if (copy[ref].o == IR_TOSTR && copy[ref].op2 == tostr_mode &&
          (copy[ref].t.irt & ~IRT_ISPHI) == IRT_STR)
        source_tostr = ref;
    for (ref = loop+1u; ref < n; ref++)
      if (copy[ref].o == IR_TOSTR && copy[ref].op2 == tostr_mode &&
          (copy[ref].t.irt & ~IRT_ISPHI) == IRT_STR) {
        body_tostr = ref;
        break;
      }
    for (ref = loop+1u; ref < n; ref++)
      if (copy[ref].o == IR_PHI && irt_type(copy[ref].t) == IRT_STR &&
          copy[ref].op1 == source_tostr &&
          copy[ref].op2 == body_tostr) {
        tostr_phi = ref;
      }
    if (source_tostr && body_tostr && tostr_phi &&
        copy[tostr_phi].op2 == body_tostr &&
        lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, body_tostr)) {
      IRIns saved_source = copy[source_tostr];
      IRIns saved_phi = copy[tostr_phi];
      copy[source_tostr].s = SPS_NONE;
      copy[source_tostr].r = RID_X0;
      assert(!lj_asm_arm64_test_scalar_math_live_across(
        &v, n, loop, body_tostr));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[source_tostr] = saved_source;
      copy[tostr_phi].op1 = body_tostr;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[tostr_phi] = saved_phi;
      copy[tostr_phi].op2 = source_tostr;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[tostr_phi] = saved_phi;
      copy[body_tostr].op2 = tostr_mode == IRTOSTR_INT ?
        IRTOSTR_NUM : IRTOSTR_INT;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[body_tostr] = T->ir[body_tostr];
      assert(lj_asm_arm64_postra_admit(&v, NULL));
      printf("TOSTR loop survival: source=%u body=%u phi=%u; volatile-source, PHI and mode mutations rejected\n",
             (unsigned)source_tostr, (unsigned)body_tostr,
             (unsigned)tostr_phi);
    }
    printf("TOSTR post-RA: mode %u/input type %u accepted; other numeric mode, CHAR, wrong input and forward ref rejected\n",
           (unsigned)tostr_mode, (unsigned)tostr_input_type);
  }
  if (mathkind && strcmp(mathkind, "math_computed_result") == 0) {
    IRRef call = 0, value = 0, phi = 0, rename = 0, alternate = 0;
    SnapNo loop_snap = 0, other_snap = 0;
    unsigned loop_snaps = 0, other_snaps = 0;
    MSize s;
    for (ref = loop+1u; ref < n; ref++)
      if (copy[ref].o == IR_CALLN && irt_type(copy[ref].t) == IRT_NUM) {
        call = ref;
        break;
      }
    for (ref = REF_FIRST; ref < loop; ref++)
      if (copy[ref].o == IR_CALLN && irt_type(copy[ref].t) == IRT_NUM) {
        value = ref;
        break;
      }
    for (ref = loop+1u; ref < n; ref++)
      if (copy[ref].o == IR_PHI && irt_type(copy[ref].t) == IRT_NUM &&
          copy[ref].op1 == value && copy[ref].op2 == call) {
        phi = ref;
        break;
      }
    for (s = 0; s < T->nsnap; s++)
      if (snap_ref_acq(&T->snap[s]) == loop) {
        loop_snap = (SnapNo)s;
        loop_snaps++;
      }
    for (s = 0; s < T->nsnap; s++)
      if (s != loop_snap) { other_snap = (SnapNo)s; other_snaps++; break; }
    for (ref = n; ref < T->nins; ref++)
      if (copy[ref].o == IR_RENAME && copy[ref].op1 == value &&
          copy[ref].op2 == loop_snap) {
        assert(rename == 0);
        rename = ref;
      }
    assert(call && value && phi && rename && loop_snaps == 1u &&
           other_snaps == 1u && loop_snap < T->nsnap &&
           call > loop && value < loop && copy[phi].op1 == value &&
           copy[phi].op2 == call && copy[rename].op1 == value &&
           copy[rename].op2 == loop_snap &&
           lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
    for (ref = REF_FIRST; ref < loop; ref++)
      if (ref != value && irt_type(copy[ref].t) == IRT_NUM &&
          (copy[ref].o == IR_SLOAD || copy[ref].o == IR_ADD ||
           copy[ref].o == IR_SUB || copy[ref].o == IR_MUL ||
           copy[ref].o == IR_DIV)) {
        alternate = ref;
        break;
      }
    assert(alternate);
    {
      IRIns saved = copy[rename];
      copy[rename].op1 = alternate;
      assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[rename] = saved;
      copy[rename].op2 = other_snap;
      assert(copy[rename].op2 != loop_snap);
      assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[rename] = saved;
      copy[rename].prev = REGSP(RID_D0, SPS_NONE);
      assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[rename] = saved;
      {
        IRIns saved_phi = copy[phi];
        copy[phi].o = IR_ADD;
        assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[phi] = saved_phi;
      }
      {
        IRIns saved_body = copy[call];
        copy[call].o = IR_FPMATH;
        assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[call] = saved_body;
        copy[call].t.irt = IRT_INT;
        assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[call] = saved_body;
      }
      copy[rename].prev = REGSP(RID_NONE, SPS_FIRST-1u);
      assert(!lj_asm_arm64_test_scalar_math_live_across(&v, n, loop, call));
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[rename] = saved;
      assert(lj_asm_arm64_postra_admit(&v, NULL));
    }
    printf("MATH computed-result CALLN=%u loop-PHI=%u rename=%u; body-call/source/snapshot/location/ordinary-use mutations rejected\n",
           (unsigned)call, (unsigned)phi, (unsigned)rename);
  }
  /* A future consumer has not yet been certified when an earlier absent
     location is examined. Its other operand must be checked before loading. */
  {
    IRRef mul = 0, use = 0;
    for (ref = REF_FIRST; ref + 1 < n; ref++)
      if (copy[ref].o == IR_MUL && irt_isnum(copy[ref].t)) {
        mul = ref;
        use = ref + 1;
        break;
      }
    if (mul) {
      copy[mul].t.irt = IRT_NUM;
      copy[mul].r = RID_INIT;
      copy[mul].s = 0;
      copy[use].o = IR_ADD;
      copy[use].t.irt = IRT_NUM;
      copy[use].op1 = mul;
      copy[use].op2 = T->nins;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mul] = T->ir[mul];
      copy[use] = T->ir[use];
    }
  }
  if (num) {
    copy[num].o = IR_ALOAD;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].o = IR_CALLS;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
  }
  copy[loop + 1].op1 = 0;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  copy[loop + 1] = T->ir[loop + 1];
  if (num) {
    copy[num].r = RID_GL;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].s = SPS_FIRST + 1; /* NUM needs an aligned pair. */
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    copy[num].op1 = num;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[num] = T->ir[num];
    if (knum) {
      copy[num].op1 = knum + 1;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[num] = T->ir[num];
    }
  }
  if (fpmath) {
    copy[fpmath].op2 = IRFPM_OTHER; /* No generic helper dispatch. */
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[fpmath] = T->ir[fpmath];
    copy[fpmath].op1 = n; /* Forward operand must fail before dereference. */
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[fpmath] = T->ir[fpmath];
  }
  if (mathcall) {
    IRIns saved = copy[mathcall];
    copy[mathcall].op2 = IRCALL_lj_prng_u64d;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = saved;
    copy[mathcall].op1 = n;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = saved;
    copy[mathcall].t.irt |= IRT_GUARD;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = saved;
  }
  if (mathcall && copy[mathcall].op2 == IRCALL_atan2) {
    IRRef arg = copy[mathcall].op1;
    IRIns saved = copy[arg];
    assert(saved.o == IR_CARG && saved.t.irt == IRT_NIL);
    copy[arg].op1 = arg;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[arg] = saved;
    copy[arg].op2 = REF_NIL;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[arg] = saved;
    copy[arg].t.irt = IRT_NUM;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[arg] = saved;
    copy[mathcall].op1 = saved.op1;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = T->ir[mathcall];
    /* A CARG ref is not a numeric value for an unrelated future use. */
    copy[mathcall].o = IR_ADD;
    copy[mathcall].op1 = saved.op1;
    copy[mathcall].op2 = arg;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = T->ir[mathcall];
    /* Future-user opcode corruption must fail before indexing IR mode. */
    copy[mathcall].o = IR__MAX;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathcall] = T->ir[mathcall];
  }
  if (mathop && (copy[mathop].o == IR_POW || copy[mathop].o == IR_LDEXP)) {
    IRIns saved = copy[mathop];
    copy[mathop].op2 = REF_NIL;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathop] = saved;
    copy[mathop].t.irt = IRT_I64;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathop] = saved;
  }

  if (mathcall || fpmath) {
    IRRef value = mathcall ? mathcall : fpmath;
    copy[value].t.irt ^= IRT_ISPHI;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[value] = T->ir[value];
  }
  if (mathcall || (mathop && (copy[mathop].o == IR_POW ||
                            copy[mathop].o == IR_LDEXP)) ||
      (fpmath && copy[fpmath].op2 >= IRFPM_LOG)) {
    IRRef call, value;
    unsigned clobbers = 0;
    for (call = REF_FIRST; call < n; call++) {
      IRIns helper = copy[call];
      if (helper.o != IR_CALLN && helper.o != IR_POW &&
          helper.o != IR_LDEXP && !(helper.o == IR_FPMATH && helper.op2 >= IRFPM_LOG)) continue;
      for (value = REF_FIRST; value < call; value++) {
        IRIns saved = copy[value];
        IRRef use, first = value < loop && loop < call ? loop+1u : call+1u;
        int live = 0, renamed = 0;
        if (saved.t.irt != IRT_NUM || saved.o == IR_FLOAD ||
            saved.o == IR_CONV || saved.o == IR_PHI) continue;
        for (use = first; use < n; use++) {
          IRMode mode = lj_ir_mode[copy[use].o];
          live |= (irm_op1(mode) == IRMref && copy[use].op1 == value) ||
                  (irm_op2(mode) == IRMref && copy[use].op2 == value);
        }
        for (use = n; use < T->nins; use++)
          renamed |= copy[use].o == IR_RENAME && copy[use].op1 == value;
        if (!live || renamed) continue;
        /* Select from SSA liveness, never from the acceptance verdict. A
           caller-saved NUM without a spill cannot survive this helper. */
        copy[value].r = RID_D0;
        copy[value].s = 0;
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[value] = saved;
        clobbers++;
      }
    }
    if (mathkind && (strcmp(mathkind, "math_helpers_composed") == 0 ||
                     strcmp(mathkind, "math_binary_helpers_composed") == 0))
      assert(clobbers != 0);
    printf("MATH caller-saved live-value corruptions=%u\n", clobbers);
  }
  if (math_builtin && mathguard) {
    const char *wrong = "random";
    GCfunc *fn;
    lua_getglobal(L, "math"); lua_getfield(L, -1, wrong);
    assert(tvisfunc(L->top - 1)); fn = funcV(L->top - 1);
    ir_kgc_store_rel(&copy[copy[mathguard].op2], obj2gco(fn));
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    ir_kgc_store_rel(&copy[copy[mathguard].op2],
                     ir_kgc_acq(&T->ir[T->ir[mathguard].op2]));
    lua_pop(L, 2);
  }
  if (mathop) {
    IRIns saved = copy[mathop];
    if (saved.o == IR_ABS || saved.o == IR_NEG) {
      copy[mathop].op2 = saved.op1; /* Mask provenance must be exact. */
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mathop] = saved;
    } else {
      copy[mathop].op1 = n; /* Forward reference must fail closed. */
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mathop] = saved;
    }
    copy[mathop].t.irt |= IRT_GUARD;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[mathop] = saved;
    if (saved.o == IR_MIN || saved.o == IR_MAX) {
      copy[mathop].r = RID_GL;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mathop] = saved;
      copy[mathop].s = irt_type(saved.t) == IRT_NUM ? SPS_FIRST + 1 : SPS_LIMIT-1;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[mathop] = saved;
    }
  }
  if (maskload) {
    IRIns saved = copy[maskload];
    MSize slot, framecap, saved_adjust = v.spadjust;
    uint8_t occupied[SPS_LIMIT] = {0};
    IRRef other;
    /* Find an aligned, otherwise free NUM spill pair within this trace's
       certified frame so rejection specifically exercises mask placement. */
    for (other = REF_FIRST; other < n; other++) {
      IRIns ins = copy[other];
      unsigned width = irt_type(ins.t) == IRT_NUM ? 2u : 1u;
      if (ra_hasspill(ins.s))
        for (slot = ins.s; slot < (MSize)ins.s + width && slot < SPS_LIMIT;
             slot++)
          occupied[slot] = 1;
    }
    assert(saved.r == RID_INIT && !ra_hasspill(saved.s) && saved.s == 0);
    copy[maskload].r = RID_D0;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[maskload] = saved;
    for (slot = (SPS_FIRST + 1u) & ~1u;
         slot + 1u < SPS_LIMIT; slot += 2u)
      if (!occupied[slot] && !occupied[slot + 1u]) break;
    assert(slot + 1u < SPS_LIMIT);
    /* Grow only the test view's legal frame so this is a valid, free NUM
       spill location; the mask-specific gate must reject it before layout. */
    framecap = ((slot + 3u) / 4u) * 4u + SPS_FIXED;
    assert(framecap >= slot + 2u && framecap <= SPS_LIMIT);
    v.spadjust = sps_scale(framecap - SPS_FIXED);
    copy[maskload].s = (uint8_t)slot;
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[maskload] = saved;
    v.spadjust = saved_adjust;
  }
  if (fpmath) {
    copy[fpmath].op2 = IRFPM_OTHER; /* No generic helper dispatch. */
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[fpmath] = T->ir[fpmath];
    copy[fpmath].op1 = n; /* Forward operand must fail before dereference. */
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    copy[fpmath] = T->ir[fpmath];
  }
  if (math_builtin && mathguard) {
    const char *wrong = "random";
    GCfunc *fn;
    lua_getglobal(L, "math"); lua_getfield(L, -1, wrong);
    assert(tvisfunc(L->top - 1)); fn = funcV(L->top - 1);
    ir_kgc_store_rel(&copy[copy[mathguard].op2], obj2gco(fn));
    assert(!lj_asm_arm64_postra_admit(&v, NULL));
    ir_kgc_store_rel(&copy[copy[mathguard].op2],
                     ir_kgc_acq(&T->ir[T->ir[mathguard].op2]));
    lua_pop(L, 2);
  }

  {
    Reg reserved[] = {RID_GL, RID_DISPATCH, RID_FP, RID_SP};
    unsigned i;
    IRRef integer = 0;
    for (ref = REF_FIRST; ref < n; ref++)
      if (irt_isint(copy[ref].t) && copy[ref].o == IR_ADDOV) {
        integer = ref;
        break;
      }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
      copy[REF_BASE].r = reserved[i];
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[REF_BASE] = T->ir[REF_BASE];
      if (integer) {
        copy[integer].r = reserved[i];
        assert(!lj_asm_arm64_postra_admit(&v, NULL));
        copy[integer] = T->ir[integer];
      }
    }
    if (num) {
      copy[num].r = RID_SINK;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[num] = T->ir[num];
      copy[num].r = RID_SUNK;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[num] = T->ir[num];
      copy[num].r = RID_NONE | RID_GL;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[num] = T->ir[num];
      copy[num].r = RID_INIT;
      copy[num].s = 0;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[num] = T->ir[num];
    }
    if (integer) {
      copy[integer].s = 1;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[integer] = T->ir[integer];
    }
    if (copy[n].o == IR_RENAME) {
      copy[n].op1 = n;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
      copy[n].op2 = T->nsnap;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
      copy[n].r = RID_GL;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[n] = T->ir[n];
    }
  }
  for (ref = REF_FIRST; ref < n; ref++) {
    if (copy[ref].o == IR_PHI && irt_isnum(copy[ref].t) &&
        !ra_hasspill(copy[ref].s) &&
        !ra_hasspill(copy[copy[ref].op1].s) &&
        !ra_hasspill(copy[copy[ref].op2].s)) {
      /* Unspilled PHIs preserve register correspondence. Dedicated numeric
      ** pressure tests cover saved inputs and the copy-spill path. */
      IRRef left = copy[ref].op1, right = copy[ref].op2;
      Reg changed = copy[ref].r == RID_D14 ? RID_D13 : RID_D14;
      copy[ref].r = changed;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[ref] = T->ir[ref];
      copy[left].r = changed;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[left] = T->ir[left];
      copy[right].r = copy[right].r == RID_D14 ? RID_D13 : RID_D14;
      assert(!lj_asm_arm64_postra_admit(&v, NULL));
      copy[right] = T->ir[right];
      break;
    }
  }
  v.spadjust += 16;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  v.spadjust = T->spadjust;
  map[0] = 0xffffffffu;
  assert(!lj_asm_arm64_postra_admit(&v, NULL));
  memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
  assert(lj_asm_arm64_postra_admit(&v, NULL));

  {
    jit_State *fake = calloc(1, sizeof(*fake));
    GCtrace semantic = *T;
    IRIns *semantic_ir = calloc(T->nins, sizeof(*semantic_ir));
    SnapEntry *semantic_map = malloc(T->nsnapmap * sizeof(*semantic_map));
    assert(semantic_ir && semantic_map);
    memcpy(semantic_ir + T->nk, T->ir + T->nk,
           (n - T->nk) * sizeof(*semantic_ir));
    memcpy(semantic_map, T->snapmap, T->nsnapmap * sizeof(*semantic_map));
    copy = semantic_ir;
    map = semantic_map;
    BCIns *pc = (BCIns *)(void *)trace_startpc_acq(T);
    BCIns saved = *pc;
    LJArm64IRReject reject;
    assert(fake);
    fake->L = L;
    fake->pt = pt;
    fake->startpc = pc;
    fake->baseslot = 1 + LJ_FR2;
    fake->loopref = loop;
    semantic.ir = copy;
    semantic.snap = snaps;
    semantic.snapmap = map;
    semantic.nins = n;
    semantic.sinktags = 0;
    la_store32_rel((uint32_t *)pc, T->startins);
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    if (mathkind && strncmp(mathkind, "integer_mod", 11) == 0)
      mutate_modulo(fake, &semantic, &v, n, loop);
    if (scalar_tostr_kind(mathkind)) {
      IRIns saved, saved_lref, saved_reset;
      IRRef source_tostr = 0, body_tostr = 0, tostr_phi = 0;
      MSize mapidx;
      MSize badsnap;
      SnapEntry saved_map;
      SnapShot saved_snap;
      assert(tostr && intref && lref && tmpreset &&
             (copy[tostr].t.irt & ~IRT_ISPHI) == IRT_STR &&
             copy[tostr].op2 == tostr_mode && copy[tostr].op1 < tostr &&
             irt_type(copy[copy[tostr].op1].t) == tostr_input_type);
      saved = copy[tostr];
      copy[tostr].op2 = tostr_mode == IRTOSTR_INT ?
        IRTOSTR_NUM : IRTOSTR_INT;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tostr] = saved;
      copy[tostr].op2 = IRTOSTR_CHAR;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tostr] = saved;
      copy[tostr].op1 = tostr_mode == IRTOSTR_INT ? REF_NIL : intref;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tostr] = saved;
      copy[tostr].op1 = n;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tostr] = saved;
      assert(copy[lref].t.irt == IRT_THREAD && copy[tmpreset].t.irt == IRT_PGC &&
             copy[tmpreset].op1 == lref);
      saved_lref = copy[lref];
      saved_reset = copy[tmpreset];
      copy[tmpreset].op2 = IRCALL_lj_buf_tmp_reset + 1u;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tmpreset] = saved_reset;
      copy[tmpreset].op1 = intref;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tmpreset] = saved_reset;
      copy[tmpreset].t.irt = IRT_INT;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[tmpreset] = saved_reset;
      copy[lref].op1 = 1;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[lref] = saved_lref;
      copy[lref].t.irt = IRT_PTR;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[lref] = saved_lref;
      for (mapidx = 0; mapidx < T->nsnapmap; mapidx++)
        if (snap_ref(map[mapidx]) == tostr) break;
      assert(mapidx < T->nsnapmap);
      saved_map = map[mapidx];
      map[mapidx] = snap_setref(saved_map, tmpreset);
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      map[mapidx] = snap_setref(saved_map, lref);
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      map[mapidx] = saved_map;
      for (badsnap = 0; badsnap < T->nsnap; badsnap++)
        if (snap_nent_acq(&snaps[badsnap]) != 0) break;
      assert(badsnap < T->nsnap);
      saved_snap = snaps[badsnap];
      snaps[badsnap].mapofs = (uint32_t)T->nsnapmap;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      snaps[badsnap] = saved_snap;
      for (ref = REF_FIRST; ref < loop; ref++)
        if (copy[ref].o == IR_TOSTR && copy[ref].op2 == tostr_mode &&
            (copy[ref].t.irt & ~IRT_ISPHI) == IRT_STR)
          source_tostr = ref;
      for (ref = loop+1u; ref < n; ref++)
        if (copy[ref].o == IR_TOSTR && copy[ref].op2 == tostr_mode &&
            (copy[ref].t.irt & ~IRT_ISPHI) == IRT_STR) {
          body_tostr = ref;
          break;
        }
      for (ref = loop+1u; ref < n; ref++)
        if (copy[ref].o == IR_PHI && irt_type(copy[ref].t) == IRT_STR &&
            copy[ref].op1 == source_tostr && copy[ref].op2 == body_tostr) {
          assert(!tostr_phi);
          tostr_phi = ref;
        }
      if (source_tostr && body_tostr && tostr_phi) {
        assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        IRIns saved_phi = copy[tostr_phi];
        copy[tostr_phi].op1 = body_tostr;
        assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        copy[tostr_phi] = saved_phi;
        copy[tostr_phi].op2 = source_tostr;
        assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        copy[tostr_phi] = saved_phi;
      }
      assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      printf("TOSTR semantic: mode %u/input type %u and loop PHI accepted; bad PHI inputs, other numeric mode, CHAR, wrong input and forward ref rejected\n",
             (unsigned)tostr_mode, (unsigned)tostr_input_type);
    }
    if (num) {
      copy[num].r = RID_SINK;
      copy[num].s = 1;
      assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
    }
    copy[REF_BASE].r = RID_GL;
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[REF_BASE] = T->ir[REF_BASE];
    copy[loop + 1].op1 = 0;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    copy[loop + 1] = T->ir[loop + 1];
    if (num) {
      copy[num].op1 = num;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
      copy[num].o = IR_ALOAD;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
      copy[num].o = IR_CALLS;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
    }
    if (num) {
      copy[num].t.irt |= IRT_GUARD;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[num] = T->ir[num];
      if (knum) {
        copy[num].op1 = knum + 1;
        assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        copy[num] = T->ir[num];
      }
    }
    if (fpmath) {
      copy[fpmath].op2 = IRFPM_OTHER;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[fpmath] = T->ir[fpmath];
      copy[fpmath].op1 = n;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[fpmath] = T->ir[fpmath];
    }
    if (mathcall) {
      IRIns saved = copy[mathcall];
      copy[mathcall].op2 = IRCALL_lj_prng_u64d;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = saved;
      copy[mathcall].op1 = n;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = saved;
      copy[mathcall].t.irt |= IRT_GUARD;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = saved;
    }
    if (mathcall && copy[mathcall].op2 == IRCALL_atan2) {
      IRRef arg = copy[mathcall].op1;
      IRIns saved = copy[arg];
      assert(saved.o == IR_CARG && saved.t.irt == IRT_NIL);
      copy[arg].op1 = arg;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[arg] = saved;
      copy[arg].op2 = REF_NIL;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[arg] = saved;
      copy[arg].t.irt = IRT_NUM;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[arg] = saved;
      copy[mathcall].op1 = saved.op1;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = T->ir[mathcall];
      /* A CARG ref is not a numeric value for an unrelated future use. */
      copy[mathcall].o = IR_ADD;
      copy[mathcall].op1 = saved.op1;
      copy[mathcall].op2 = arg;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = T->ir[mathcall];
      /* Future-user opcode corruption must fail before indexing IR mode. */
      copy[mathcall].o = IR__MAX;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathcall] = T->ir[mathcall];
    }
    if (mathop && (copy[mathop].o == IR_POW || copy[mathop].o == IR_LDEXP)) {
      IRIns saved = copy[mathop];
      copy[mathop].op2 = REF_NIL;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathop] = saved;
      copy[mathop].t.irt = IRT_I64;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathop] = saved;
    }

    if (mathcall || fpmath) {
      IRRef value = mathcall ? mathcall : fpmath;
      copy[value].t.irt ^= IRT_ISPHI;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[value] = T->ir[value];
    }
    if (math_builtin && mathguard) {
      const char *wrong = "random";
      GCfunc *fn;
      lua_getglobal(L, "math"); lua_getfield(L, -1, wrong);
      assert(tvisfunc(L->top - 1)); fn = funcV(L->top - 1);
      ir_kgc_store_rel(&copy[copy[mathguard].op2], obj2gco(fn));
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      ir_kgc_store_rel(&copy[copy[mathguard].op2],
                       ir_kgc_acq(&T->ir[T->ir[mathguard].op2]));
      lua_pop(L, 2);
    }
    if (mathop) {
      IRIns saved = copy[mathop];
      if (saved.o == IR_ABS || saved.o == IR_NEG) {
        copy[mathop].op2 = saved.op1;
        assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        copy[mathop] = saved;
      } else {
        copy[mathop].op1 = n;
        assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
        copy[mathop] = saved;
      }
      copy[mathop].t.irt |= IRT_GUARD;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[mathop] = saved;
    }
    if (fpmath) {
      copy[fpmath].op2 = IRFPM_OTHER;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[fpmath] = T->ir[fpmath];
      copy[fpmath].op1 = n;
      assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
      copy[fpmath] = T->ir[fpmath];
    }
    fake->framedepth = 1;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    assert(lj_asm_arm64_postra_admit(&v, NULL));
    fake->framedepth = 0;
    semantic.link = 0;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    semantic.link = T->link;
    map[0] = 0xffffffffu;
    assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    memcpy(map, T->snapmap, T->nsnapmap * sizeof(*map));
    assert(lj_asm_arm64_ir_admit(fake, &semantic, &reject));
    la_store32_rel((uint32_t *)pc, saved);
    free(fake);
    free(semantic_ir);
    free(semantic_map);
    copy = (IRIns *)v.ir;
    map = (SnapEntry *)v.snapmap;
    puts("SEMANTIC poll, forward/payload refs, heap/call opcodes, flags, "
         "frame/link, snapshot rejected");
  }
  free(map);
  free(snaps);
  assert(munmap(guarded, mapsize + pagesize) == 0);
  (void)L;
  puts("MUTATIONS poll, register class, NUM spill alignment, forward/payload ref, "
       "frame, snapshot rejected");
}

static int reset(lua_State *L)
{
  (void)L;
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  return 0;
}
static int check(lua_State *L)
{
  const char *mathkind = lua_gettop(L) ? lua_tostring(L, 1) : NULL;
  if (mathkind && strncmp(mathkind, "math_", 5) != 0 &&
      strcmp(mathkind, "numeric_neg") != 0 &&
      strncmp(mathkind, "integer_mod", 11) != 0 &&
      !scalar_tostr_kind(mathkind)) mathkind = NULL;
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef ref;
  unsigned ns = 0, nnum = 0, ntab = 0, ren = 0;
  assert(T && trace_runnable_acq(T, 1));
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_RENAME)
      ren++;
    if (ra_hasspill(ir->s)) {
      ns++;
      if (irt_isnum(ir->t))
        nnum++;
      if (irt_istab(ir->t))
        ntab++;
    }
  }
  printf("NATIVE publish=%u exits=%u last_exit=%u spadjust=%u spills=%u "
         "numeric=%u tab=%u rename=%u\n",
         lj_trace_test_root_entry_publishes(), lj_trace_test_exit_calls(),
         lj_trace_test_last_exitno(), T->spadjust, ns, nnum, ntab, ren);
  assert(lj_trace_test_root_entry_publishes() > 0 &&
         lj_trace_test_exit_calls() > 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  mutate_layout(L, T, mathkind);
  return 0;
}
typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ | TGF_STOPREQ_FRESH));
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
        LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&publisher->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(publisher->g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(publisher->tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(publisher->g) == 0);
  assert(lj_tg_reqmask_acq(publisher->tg) == 0);
  assert(lj_tg_poll_acq(publisher->tg) == 0);
  if (lj_tg_load_jit_base(publisher->tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(publisher->tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    gc2_hs_actions_rel(publisher->g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(publisher->g, 1);
    gc2_hs_epoch_rel(publisher->g, publisher->epoch + 1u);
    lj_tg_reqmask_rel(publisher->tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(publisher->tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void native_idle(lua_State *L, int32_t vmstate)
{
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == vmstate);
}
static int invoke(lua_State *L, int n)
{
  int i;
  lua_pushvalue(L, 1);
  for (i = 3; i <= n; i++)
    lua_pushvalue(L, i);
  return lua_pcall(L, n - 2, 1, 0);
}
static void equal_result(lua_State *L)
{
  if (lua_type(L, -1) == LUA_TSTRING && lua_type(L, 2) == LUA_TSTRING) {
    size_t gotlen, wantlen;
    const char *got = lua_tolstring(L, -1, &gotlen);
    const char *want = lua_tolstring(L, 2, &wantlen);
    if (gotlen != wantlen || memcmp(got, want, gotlen) != 0)
      fprintf(stderr, "string result length %lu expected %lu\n",
              (unsigned long)gotlen, (unsigned long)wantlen);
    assert(gotlen == wantlen && memcmp(got, want, gotlen) == 0);
    lua_pop(L, 1);
    return;
  }
  if (lua_tonumber(L, -1) != lua_tonumber(L, 2))
    fprintf(stderr, "result %.17g expected %.17g\n", lua_tonumber(L, -1),
            lua_tonumber(L, 2));
  assert(lua_tonumber(L, -1) == lua_tonumber(L, 2));
  lua_pop(L, 1);
}
static int lifecycle(lua_State *L)
{
  int n = lua_gettop(L), status;
  GCtrace *T = traceref_safe(L2J(L), 1);
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  int32_t vmstate = lj_tg_vmstate_load_acq(tg);
  IRRef ref, loop = 0;
  SnapNo s, loopsnap = (SnapNo)~0u, finalexit;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  assert(T && trace_runnable_acq(T, 1));
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_LOOP)
      loop = ref;
  for (s = 0; s < T->nsnap; s++)
    if (T->snap[s].ref == loop)
      loopsnap = s;
  assert(loop && loopsnap != (SnapNo)~0u);
  finalexit = T->nsnap - 1;

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher =
      (PostAdmissionPublisher){g, tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
                        &publisher) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == 0);
  equal_result(L);
  assert(publisher.published && lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_exit_calls() == 2 &&
         lj_trace_test_first_exitno() == loopsnap);
  assert(lj_trace_test_last_exitno() == finalexit &&
         lj_tg_profile_request_acq(tg) == 0);
  native_idle(L, vmstate);

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher =
      (PostAdmissionPublisher){g, tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
                        &publisher) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown"));
  lua_pop(L, 1);
  assert(publisher.published && lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1 &&
         lj_trace_test_last_exitno() == loopsnap);
  assert(gc2_hs_epoch_acq(g) == epoch + 1 &&
         lj_tg_hs_epoch_ack_acq(tg) == epoch + 1);
  assert(!gc2_hs_pending_acq(g) && !lj_tg_reqmask_acq(tg) &&
         !lj_tg_poll_acq(tg));
  assert(lj_tg_flags_acq(tg) & TGF_STOPREQ);
  native_idle(L, vmstate);
  clear_stopreq(tg);

  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_last_exitno() == finalexit);
  native_idle(L, vmstate);
  lua_gc(L, LUA_GCCOLLECT, 0);
  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1);
  native_idle(L, vmstate);
  puts("LIFECYCLE post-admission profile, STOPREQ unwind, resumed entry, full "
       "GC passed");
  return 0;
}
static int changed(lua_State *L)
{
  int n = lua_gettop(L);
  int32_t vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  GCtrace *T = traceref_safe(L2J(L), 1);
  assert(T);
  reset(L);
  assert(invoke(L, n) == 0);
  equal_result(L);
  assert(lj_trace_test_root_entry_publishes() > 0 &&
         lj_trace_test_exit_calls() > 0);
  assert(lj_trace_test_first_exitno() < T->nsnap - 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  native_idle(L, vmstate);
  printf("CHANGED input guard exit %u, numeric snapshot restored\n",
         lj_trace_test_first_exitno());
  return 0;
}
static int flush(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  GCproto *pt;
  const BCIns *pc;
  BCIns ins;
  assert(T);
  pt = trace_startpt_acq(T);
  pc = trace_startpc_acq(T);
  ins = T->startins;
  assert(lj_trace_flushall_gc(L) == 0);
  assert(la_load32_acq((const uint32_t *)pc) == ins && !proto_trace_acq(pt));
  T = traceref_safe(L2J(L), 1);
  assert(!T || !trace_runnable_acq(T, 1));
  puts("FLUSH root bytecode restored and native trace retired");
  return 0;
}

static int fma(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef ref;
  unsigned missing = 0, fused = 0, i;
  const MCode *mc = trace_mcode_acq(T);
  assert(T);
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_MUL && ra_noreg(T->ir[ref].r) && !T->ir[ref].s)
      missing++;
  for (i = 0; i < T->szmcode / sizeof(MCode); i++)
    if ((mc[i] & 0xffe08000u) == A64I_FMADDd ||
        (mc[i] & 0xffe08000u) == A64I_FMSUBd ||
        (mc[i] & 0xffe08000u) == A64I_FNMSUBd)
      fused++;
  assert(missing && fused);
  printf(
      "FMA absent multiplication locations=%u emitted fused operations=%u\n",
      missing, fused);
  return 0;
}
static int phis(lua_State *L)
{
  GCtrace *T = traceref_safe(L2J(L), 1);
  IRRef r, loop = 0;
  unsigned num = 0, early = 0, duplicate = 0, renames = 0;
  assert(T && trace_runnable_acq(T, 1));
  for (r = REF_FIRST; r < T->nins; r++) {
    IRIns p = T->ir[r];
    if (p.o == IR_LOOP)
      loop = r;
    if (p.o == IR_RENAME)
      renames++;
    if (p.o == IR_PHI && irt_isnum(p.t)) {
      IRRef q;
      num++;
      early += p.op2 < loop;
      for (q = REF_FIRST; q < r; q++)
        if (T->ir[q].o == IR_PHI && T->ir[q].op2 == p.op2)
          duplicate++;
    }
  }
  printf("PHI numeric=%u earlier_rhs=%u duplicate_rhs=%u renames=%u\n",
         num, early, duplicate, renames);
  assert(num >= 2 && renames);
  assert(lua_toboolean(L, 1) ? (num == 2 && duplicate == 0) : early);
  return 0;
}
static int negative(lua_State *L)
{
  jit_State *J = L2J(L);
  TraceNo no;
  for (no = 1; no < trace_sizetrace_acq(J); no++)
    assert(!trace_runnable_acq(traceref_safe(J, no), no));
  assert(!lj_trace_test_root_entry_publishes());
  assert(!lj_trace_test_exit_calls());
  puts("NEGATIVE unsupported path stayed interpreted");
  return 0;
}
int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, reset);
  lua_setglobal(L, "probe_reset");
  lua_pushcfunction(L, check);
  lua_setglobal(L, "probe_check");
  lua_pushcfunction(L, lifecycle);
  lua_setglobal(L, "probe_lifecycle");
  lua_pushcfunction(L, changed);
  lua_setglobal(L, "probe_changed");
  lua_pushcfunction(L, flush);
  lua_setglobal(L, "probe_flush");
  lua_pushcfunction(L, fma);
  lua_setglobal(L, "probe_fma");
  lua_pushcfunction(L, phis);
  lua_setglobal(L, "probe_phis");
  lua_pushcfunction(L, negative);
  lua_setglobal(L, "probe_negative");
  assert(argc == 1 || argc == 2);
  if (luaL_dofile(L, argc == 2 ? argv[1]
                               : "tests/t-arm64-jit-scalar-capability.lua")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    return 1;
  }
  lua_close(L);
  return 0;
}

#else
int main(void)
{
  puts(
      "t-arm64-jit-scalar-capability SKIP: requires experimental macOS ARM64");
  return 0;
}
#endif
