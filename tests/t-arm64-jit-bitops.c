/* Native ARM64 bit operations and independent semantic/allocation negatives. */
#include <stdio.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_frame.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

static const struct {
  const char *name;
  IROp op;
} bitops[] = {
  {"BNOT", IR_BNOT}, {"BSWAP", IR_BSWAP}, {"BAND", IR_BAND},
  {"BOR", IR_BOR}, {"BXOR", IR_BXOR}, {"BSHL", IR_BSHL},
  {"BSHR", IR_BSHR}, {"BSAR", IR_BSAR}, {"BROR", IR_BROR},
  {"BROL", IR_BROL}, {"TOBIT", IR_TOBIT}, {"NEG", IR_NEG},
  {"ADD", IR_ADD}, {"SUB", IR_SUB}
};
static uint64_t covered_ops;
static unsigned machine_ops, snapshot_conversions, fused_integers, integer_spills;
static unsigned cross_loop_fusions;
static unsigned last_fused, last_fusion_mask;

static unsigned bitop_machine_mask(GCtrace *T)
{
  const MCode *mc = trace_mcode_acq(T);
  MSize i, count = T->szmcode / sizeof(MCode);
  unsigned mask = 0;
  for (i = 0; i < count; i++) {
    uint32_t word = mc[i];
    if ((word & 0xff2003e0u) == A64I_MVNw) mask |= 1u;
    if ((word & 0xfffffc00u) == A64I_REVw) mask |= 2u;
    if ((word & 0xff200000u) == A64I_ANDw) mask |= 4u;
    if ((word & 0xff200000u) == A64I_ORRw) mask |= 8u;
    if ((word & 0xff200000u) == A64I_EORw) mask |= 16u;
    if ((word & 0xffe0fc00u) == A64I_SHRw) mask |= 32u;
    if ((word & 0xffe0fc00u) == (A64I_SHRw | (1u << 10))) mask |= 64u;
    if ((word & 0xffe0fc00u) == (A64I_SHRw | (2u << 10))) mask |= 128u;
    if ((word & 0xffe0fc00u) == (A64I_SHRw | (3u << 10))) mask |= 256u;
    if ((word & 0xffe0ffe0u) == A64I_NEGw) mask |= 512u;
  }
  return mask;
}

/* Instruction families paired with an actually unlocated IR producer. */
static unsigned bitop_fusion_mask(GCtrace *T)
{
  const MCode *mc = trace_mcode_acq(T);
  MSize i, count = T->szmcode / sizeof(MCode);
  unsigned mask = 0;
  for (i = 0; i < count; i++) {
    uint32_t w = mc[i], logic = w & 0xff200000u;
    unsigned shifted = (w & 0x00c0fc00u) != 0;
    if (logic == (A64I_ORRw | A64I_ON) && ((w >> 5) & 31u) != 31u) mask |= 1u;
    if ((logic == A64I_EORw || logic == A64I_ORRw || logic == A64I_ANDw) && shifted) mask |= 2u;
    if ((w & 0xff2003e0u) == A64I_MVNw && shifted) mask |= 4u;
    if ((w & 0xffc00000u) == A64I_UBFMw) mask |= 8u;
    if ((w & 0xffe00000u) == A64I_EXTRw &&
        ((w >> 5) & 31u) != ((w >> 16) & 31u)) mask |= 16u;
    if ((w & 0xff20001fu) == A64I_TSTw ||
        (w & 0xfe000000u) == 0x36000000u) mask |= 32u;
    if ((w & 0xff200000u) == A64I_ADDw && shifted) mask |= 64u;
    if ((w & 0xff200000u) == A64I_SUBw && shifted) mask |= 128u;
    if ((w & 0xffc00000u) == A64I_SBFMw) mask |= 256u;
    if (logic == (A64I_EORw | A64I_ON)) mask |= 512u;
  }
  return mask;
}

typedef struct BitopView {
  lua_State *L;
  jit_State *J;
  GCtrace *T;
  IRIns *post_ir;
  IRIns *sem_ir;
  SnapEntry *post_map;
  SnapEntry *sem_map;
  LJArm64PostRAView post;
  jit_State *fake;
  GCtrace sem;
  GCproto *pt;
  BCIns *pc;
  BCIns savedpc;
  size_t pagesize;
  size_t mapsize;
  void *guarded;
  void *sem_guarded;
  IRRef semantic_nins;
} BitopView;

static IROp required_op(const char *name, size_t n)
{
  unsigned i;
  for (i = 0; i < sizeof(bitops)/sizeof(bitops[0]); i++)
    if (strlen(bitops[i].name) == n && !memcmp(name, bitops[i].name, n))
      return bitops[i].op;
  return IR__MAX;
}

static int op_is_int_result(const IRIns *ir)
{
  return irt_isint(ir->t);
}

static int required_ops_present(lua_State *L, GCtrace *T, const char *list)
{
  char *copy = strdup(list), *cursor, *name;
  IRRef r;
  assert(copy);
  cursor = copy;
  while ((name = strsep(&cursor, ",")) != NULL) {
    size_t n = strlen(name);
    IROp op;
    int found = 0;
    while (n && (name[n-1] == ' ' || name[n-1] == '\t')) name[--n] = '\0';
    while (*name == ' ' || *name == '\t') { name++; n--; }
    if (!n) continue;
    op = required_op(name, n);
    if (op == IR__MAX) {
      fprintf(stderr, "unknown required IR opcode: %.*s\n", (int)n, name);
      abort();
    }
    for (r = REF_FIRST; r < T->nins; r++) {
      if (T->ir[r].o == op && op_is_int_result(&T->ir[r])) {
        found = 1;
      }
    }
    if (!found) {
      fprintf(stderr, "trace %u lacks integer IR_%.*s\n",
              (unsigned)T->traceno, (int)n, name);
      free(copy);
      return 0;
    }
  }
  {
    unsigned i;
    for (r = REF_FIRST; r < T->nins; r++) {
      for (i = 0; i < sizeof(bitops)/sizeof(bitops[0]); i++)
        if (T->ir[r].o == bitops[i].op && op_is_int_result(&T->ir[r]))
          covered_ops |= UINT64_C(1) << i;
    }
  }
  free(copy);
  UNUSED(L);
  return 1;
}

static void bitop_view_init(BitopView *v, lua_State *L, GCtrace *T)
{
  size_t irbytes;
  memset(v, 0, sizeof(*v));
  v->L = L;
  v->J = L2J(L);
  v->T = T;
  v->pt = trace_startpt_acq(T);
  v->pc = (BCIns *)(void *)trace_startpc_acq(T);
  assert(v->pt && v->pc);
  irbytes = (size_t)T->nins * sizeof(IRIns);
  v->pagesize = (size_t)sysconf(_SC_PAGESIZE);
  assert(v->pagesize && (v->pagesize & (v->pagesize - 1u)) == 0);
  v->mapsize = (irbytes + v->pagesize - 1u) & ~(v->pagesize - 1u);
  v->guarded = mmap(NULL, v->mapsize + v->pagesize,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  assert(v->guarded != MAP_FAILED);
  assert(mprotect((char *)v->guarded + v->mapsize, v->pagesize, PROT_NONE) == 0);
  v->post_ir = (IRIns *)((char *)v->guarded + v->mapsize - irbytes);
  v->sem_guarded = mmap(NULL, v->mapsize + v->pagesize,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  assert(v->sem_guarded != MAP_FAILED);
  assert(mprotect((char *)v->sem_guarded + v->mapsize,
                  v->pagesize, PROT_NONE) == 0);
  v->sem_ir = (IRIns *)((char *)v->sem_guarded + v->mapsize - irbytes);
  v->post_map = malloc(T->nsnapmap * sizeof(*v->post_map));
  v->sem_map = malloc(T->nsnapmap * sizeof(*v->sem_map));
  v->fake = calloc(1, sizeof(*v->fake));
  assert(v->post_ir && v->post_map && v->sem_ir && v->sem_map && v->fake);
  /* Copy constants too: TOBIT's exact KNUM bias is mutation-tested below. */
  memcpy(v->post_ir + T->nk, T->ir + T->nk,
         (size_t)(T->nins - T->nk) * sizeof(IRIns));
  memcpy(v->sem_ir + T->nk, T->ir + T->nk,
         (size_t)(T->nins - T->nk) * sizeof(IRIns));
  memcpy(v->post_map, T->snapmap, T->nsnapmap * sizeof(*v->post_map));
  memcpy(v->sem_map, T->snapmap, T->nsnapmap * sizeof(*v->sem_map));
  v->post.ir = v->post_ir;
  v->post.owner = T;
  v->post.snap = T->snap;
  v->post.snapmap = v->post_map;
  v->post.proto_bc = proto_bc(v->pt);
  v->post.proto = v->pt;
  v->post.startpc = v->pc;
  v->post.nins = T->nins;
  v->post.nk = T->nk;
  v->post.nsnap = T->nsnap;
  v->post.nsnapmap = T->nsnapmap;
  v->post.spadjust = T->spadjust;
  v->post.proto_sizebc = v->pt->sizebc;
  v->post.proto_numparams = v->pt->numparams;
  v->post.root_topslot = T->topslot;
  v->post.startins = T->startins;
  v->sem = *T;
  v->sem.ir = v->sem_ir;
  v->sem.snapmap = v->sem_map;
  v->sem.sinktags = 0;
  v->fake->L = L;
  v->fake->pt = v->pt;
  v->fake->startpc = v->pc;
  v->fake->baseslot = 1 + LJ_FR2;
  v->fake->loopref = 0;
  for (IRRef r = REF_FIRST; r < T->nins; r++)
    if (T->ir[r].o == IR_LOOP) {
      v->fake->loopref = r;
      break;
    }
  assert(v->fake->loopref);
  v->savedpc = *v->pc;
  /* Semantic proof is over the original root bytecode, never its patched JLOOP. */
  la_store32_rel((uint32_t *)(void *)v->pc, T->startins);
  assert(lj_asm_arm64_postra_admit(&v->post, &v->semantic_nins));
  v->sem.nins = v->semantic_nins;
  v->fake->loopref = 0;
  for (IRRef r = REF_FIRST; r < v->semantic_nins; r++)
    if (v->sem_ir[r].o == IR_LOOP) {
      v->fake->loopref = r;
      break;
    }
  assert(v->fake->loopref);
  assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
}

static void bitop_view_free(BitopView *v)
{
  la_store32_rel((uint32_t *)(void *)v->pc, v->savedpc);
  free(v->post_map);
  free(v->sem_map);
  free(v->fake);
  assert(munmap(v->guarded, v->mapsize + v->pagesize) == 0);
  assert(munmap(v->sem_guarded, v->mapsize + v->pagesize) == 0);
}

static IRRef find_int_op(const IRIns *ir, IRRef nins, IROp op)
{
  IRRef r;
  for (r = REF_FIRST; r < nins; r++)
    if (ir[r].o == op && irt_isint(ir[r].t))
      return r;
  return 0;
}

static void require_both_reject(BitopView *v)
{
  assert(!lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
  assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
}

static void mutate_admission(BitopView *v, const char *required)
{
  char *names = strdup(required), *cursor, *name;
  IRIns original;
  assert(names);
  cursor = names;
  while ((name = strsep(&cursor, ",")) != NULL) {
    size_t len = strlen(name);
    IROp op;
    IRRef r;
    while (len && (name[len-1] == ' ' || name[len-1] == '\t'))
      name[--len] = '\0';
    while (*name == ' ' || *name == '\t') { name++; len--; }
    if (!len) continue;
    op = required_op(name, len);
    assert(op != IR__MAX);
    r = find_int_op(v->post_ir, v->T->nins, op);
    assert(r != 0);
    original = v->T->ir[r];

    v->post_ir[r].o = IR_ALOAD;
    v->sem_ir[r].o = IR_ALOAD;
    require_both_reject(v);
    v->post_ir[r] = v->sem_ir[r] = original;

    /* Exact bounded backward-operand requirement. */
    v->post_ir[r].op1 = (IRRef1)v->T->nins;
    v->sem_ir[r].op1 = (IRRef1)v->T->nins;
    require_both_reject(v);
    v->post_ir[r] = v->sem_ir[r] = original;

    /* Semantic admission deliberately ignores allocated locations; the
    ** independent post-RA pass rejects the reserved-register mutation. */
    v->sem_ir[r].r = RID_GL;
    assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
    v->post_ir[r].r = RID_GL;
    assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
    v->post_ir[r] = v->sem_ir[r] = original;

    v->post_ir[r].t.irt = (uint8_t)((original.t.irt & ~IRT_TYPE) |
        (irt_isint(original.t) ? IRT_NUM : IRT_INT));
    v->sem_ir[r].t.irt = v->post_ir[r].t.irt;
    require_both_reject(v);
    v->post_ir[r] = v->sem_ir[r] = original;

    v->post_ir[r].t.irt = (uint8_t)(original.t.irt | IRT_GUARD);
    v->sem_ir[r].t.irt = v->post_ir[r].t.irt;
    require_both_reject(v);
    v->post_ir[r] = v->sem_ir[r] = original;

    if (op == IR_BROR) {
      v->post_ir[r].o = IR_BROL;
      v->sem_ir[r].o = IR_BROL;
      require_both_reject(v);
      v->post_ir[r] = v->sem_ir[r] = original;
    }
    if (irm_op2(lj_ir_mode[op]) == IRMref) {
      v->post_ir[r].op2 = (IRRef1)v->T->nins;
      v->sem_ir[r].op2 = (IRRef1)v->T->nins;
      require_both_reject(v);
      v->post_ir[r] = v->sem_ir[r] = original;
    } else if (irm_op2(lj_ir_mode[op]) == IRMnone) {
      /* Unary opcodes must not acquire a forged second operand. */
      v->post_ir[r].op2 = 1;
      v->sem_ir[r].op2 = 1;
      require_both_reject(v);
      v->post_ir[r] = v->sem_ir[r] = original;
    }

    /* The independent physical certificate rejects a spill slot below the
    ** first legal spill, regardless of the operation's normal allocation. */
    v->post_ir[r].s = 1;
    assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
    v->post_ir[r] = original;
    v->sem_ir[r] = original;

    /* BSWAP and TOBIT cannot be erased by the backend's fused integer
    ** addressing cases. They are live in the fixture, so a locationless
    ** allocation must fail the post-RA certificate. */
    if ((op == IR_BSWAP || op == IR_TOBIT) &&
        (!ra_noreg(original.r) || ra_hasspill(original.s))) {
      v->post_ir[r].r = RID_INIT;
      v->post_ir[r].s = SPS_NONE;
      assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
      v->post_ir[r] = original;
    }
  }
  free(names);

  /* If this trace contains a dynamic TOBIT, its canonical bias must fail both
  ** proofs when perturbed. Other bitop traces legitimately have no TOBIT. */
  {
    IRRef r, kref = 0;
    int has_tobit = 0;
    uint64_t bits = 0;
    for (r = REF_FIRST; r < v->T->nins; r++)
      if (v->T->ir[r].o == IR_TOBIT) {
        has_tobit = 1;
        if (!irref_isk(v->T->ir[r].op2))
          continue;
        IRRef k = v->T->ir[r].op2;
        if (v->T->ir[k].o == IR_KNUM) {
          kref = k;
          bits = ir_knum(v->T->ir + k)->u64;
          break;
        }
      }
    if (!has_tobit) {
      /* No bias mutation applies to a trace without this operation. */
    } else if (kref && bits == UINT64_C(0x4338000000000000)) {
      ir_knum(v->post_ir + kref)->u64 ^= 1u;
      ir_knum(v->sem_ir + kref)->u64 ^= 1u;
      require_both_reject(v);
      ir_knum(v->post_ir + kref)->u64 ^= 1u;
      ir_knum(v->sem_ir + kref)->u64 ^= 1u;
    } else {
      fprintf(stderr, "TOBIT trace lacks the canonical 0x4338000000000000 bias\n");
      abort();
    }
  }
  assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
  assert(lj_asm_arm64_postra_admit(&v->post, NULL));
}

static RegSP snapshot_location(BitopView *v, IRRef ref, SnapNo snap)
{
  IRRef ren;
  RegSP rs = v->post_ir[ref].prev;
  for (ren = v->T->nins; ren-- > v->semantic_nins;) {
    IRIns p = v->post_ir[ren];
    if (p.o != IR_RENAME) break;
    if (p.op1 == ref && p.op2 <= snap) rs = p.prev;
  }
  return rs;
}

static void mutate_snapshots(BitopView *v)
{
  IRRef ref;
  SnapNo snap;
  unsigned remat = 0, fused = 0, spills = 0;
  for (ref = REF_FIRST; ref < v->semantic_nins; ref++) {
    IRIns p = v->post_ir[ref];
    if (irt_isint(p.t) && ra_hasspill(p.s)) spills++;
    if (p.t.irt == IRT_INT && ra_noreg(p.r) && !ra_hasspill(p.s) &&
        ((p.o >= IR_BNOT && p.o <= IR_BROR) || p.o == IR_ADD)) {
      IRRef use;
      for (use = ref+1; use < v->semantic_nins; use++)
        if (v->post_ir[use].op1 == ref || v->post_ir[use].op2 == ref) {
          IRIns original = p;
          fused++;
          /* Same type and valid operand, but BSWAP cannot disappear into
          ** these consumers. Semantic acceptance must not certify layout. */
          v->post_ir[ref].o = v->sem_ir[ref].o = IR_BSWAP;
          v->post_ir[ref].op2 = v->sem_ir[ref].op2 = 0;
          assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
          assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
          v->post_ir[ref] = v->sem_ir[ref] = original;
          break;
        }
    }
  }
  for (snap = 0; snap < v->T->nsnap; snap++) {
    SnapShot *sp = &v->T->snap[snap];
    MSize j;
    for (j = 0; j < sp->nent; j++) {
      MSize at = sp->mapofs+j;
      SnapEntry sn = v->post_map[at];
      IRRef r = snap_ref(sn), candidate;
      IRIns p;
      RegSP rs;
      if (irref_isk(r) || (sn & 0x00ff0000u)) continue;
      p = v->post_ir[r];
      rs = snapshot_location(v, r, snap);
      if (p.o == IR_CONV && p.t.irt == IRT_NUM &&
          p.op2 == IRCONV_NUM_INT && ra_noreg(regsp_reg(rs)) &&
          !ra_hasspill(regsp_spill(rs))) {
        IRIns original = p;
        remat++;
        /* Only the exact stock INT-to-NUM rematerialization is permitted. */
        v->post_ir[r].op2 = v->sem_ir[r].op2 = IRCONV_NUM_INT | IRCONV_CHECK;
        require_both_reject(v);
        v->post_ir[r] = v->sem_ir[r] = original;
      }
      /* A fused result can be a valid semantic snapshot source, but it
      ** cannot be restored without a real location. Do not let the NUM_INT
      ** rematerialization exception erase that independent requirement. */
      for (candidate = REF_FIRST; candidate < sp->ref; candidate++) {
        IRIns q = v->post_ir[candidate];
        if (q.t.irt == IRT_INT && q.o >= IR_BNOT && q.o <= IR_BROR &&
            ra_noreg(q.r) && !ra_hasspill(q.s)) {
          v->post_map[at] = v->sem_map[at] =
              (sn & ~0xffffu) | candidate;
          assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
          assert(!lj_asm_arm64_postra_admit(&v->post, NULL));
          v->post_map[at] = v->sem_map[at] = sn;
          break;
        }
      }
    }
  }
  snapshot_conversions += remat;
  fused_integers += fused;
  last_fused = fused;
  integer_spills += spills;
  printf("BITOPS layout fused=%u snapshot_conversions=%u integer_spills=%u frame=%u\n",
         fused, remat, spills, v->T->spadjust);
  assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
  assert(lj_asm_arm64_postra_admit(&v->post, NULL));
}

/* The first recorded iteration and the optimized loop body each contain
** shifts. A clone may remain valid scalar IR when the body starts consuming
** an unlocated first-iteration shift, but asm_fuseopm cannot fuse across LOOP.
** Change only that operand: snapshots, PHIs, locations and frame stay intact. */
static void mutate_cross_loop_fusion(BitopView *v)
{
  IRRef loop = v->fake->loopref, source, consumer;
  for (source = REF_FIRST; source < loop; source++) {
    IRIns p = v->post_ir[source];
    IRRef use;
    int preloop_use = 0;
    if (p.o != IR_BSHL || p.t.irt != IRT_INT ||
        !irref_isk(p.op2) || ra_hasreg(p.r) || ra_hasspill(p.s)) continue;
    for (use = source+1u; use < loop; use++) {
      IRIns q = v->post_ir[use];
      if (q.o == IR_BXOR && (q.op1 == source || q.op2 == source))
        preloop_use = 1;
    }
    if (!preloop_use) continue;
    for (consumer = loop+1u; consumer < v->semantic_nins; consumer++) {
      IRIns q = v->post_ir[consumer], shifted;
      IRRef ref;
      int accepted, operand1;
      if (q.o != IR_BXOR || !irt_isint(q.t)) continue;
      operand1 = q.op1 > loop && q.op1 < consumer &&
                 v->post_ir[q.op1].o == IR_BSHL;
      ref = operand1 ? q.op1 : q.op2;
      if (ref <= loop || ref >= consumer) continue;
      shifted = v->post_ir[ref];
      if (shifted.o != IR_BSHL || shifted.t.irt != IRT_INT ||
          shifted.op2 != p.op2 || ra_hasreg(shifted.r) ||
          ra_hasspill(shifted.s)) continue;
      if (operand1)
        v->post_ir[consumer].op1 = v->sem_ir[consumer].op1 = (IRRef1)source;
      else
        v->post_ir[consumer].op2 = v->sem_ir[consumer].op2 = (IRRef1)source;
      assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
      accepted = lj_asm_arm64_postra_admit(&v->post, NULL);
      if (accepted)
        fprintf(stderr, "cross-loop fusion accepted: source=%u loop=%u use=%u\n",
                (unsigned)source, (unsigned)loop, (unsigned)consumer);
      assert(!accepted);
      v->post_ir[consumer] = v->sem_ir[consumer] = q;
      assert(lj_asm_arm64_ir_admit(v->fake, &v->sem, NULL));
      assert(lj_asm_arm64_postra_admit(&v->post, NULL));
      cross_loop_fusions++;
      return;
    }
  }
}

static int fused_check(lua_State *L)
{
  unsigned required = (unsigned)luaL_checkinteger(L, 1);
  assert(last_fused > 0 && (last_fusion_mask & required) == required);
  printf("BITOPS fusion instruction mask=%u required=%u\n", last_fusion_mask, required);
  return 0;
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
  GCfunc *fn;
  GCproto *pt;
  GCtrace *T;
  const char *required = luaL_checkstring(L, 2);
  BitopView v;
  assert(lua_isfunction(L, 1));
  fn = funcV(L->top - 2);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  T = traceref_safe(L2J(L), proto_trace_acq(pt));
  if (!T || !trace_runnable_acq(T, T->traceno))
    fprintf(stderr, "bitops trace unavailable: aborts=%u error=%u\n",
            lj_trace_test_abort_count(), (unsigned)lj_trace_test_last_abort_error());
  assert(T && trace_runnable_acq(T, T->traceno));
  assert(trace_startpt_acq(T) == pt);
  assert(bc_op(T->startins) == BC_LOOP || bc_op(T->startins) == BC_FORL);
  assert(required_ops_present(L, T, required));
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1 &&
         lj_trace_test_first_exitno() == T->nsnap-1u &&
         lj_trace_test_last_exitno() == T->nsnap-1u);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL &&
         lj_tg_in_native_acq(L2TG(L)) == 0);
  bitop_view_init(&v, L, T);
  mutate_admission(&v, required);
  mutate_snapshots(&v);
  mutate_cross_loop_fusion(&v);
  machine_ops |= bitop_machine_mask(T);
  last_fusion_mask = bitop_fusion_mask(T);
  bitop_view_free(&v);
  printf("BITOPS native trace=%u required=%s publishes=%u exits=%u "
         "first_exit=%u last_exit=%u snapshots=%u; "
         "semantic/postRA mutations rejected\n", (unsigned)T->traceno,
         required, lj_trace_test_root_entry_publishes(),
         lj_trace_test_exit_calls(), lj_trace_test_first_exitno(),
         lj_trace_test_last_exitno(), T->nsnap);
  return 0;
}

static int invoke(lua_State *L, int n)
{
  int i;
  lua_pushvalue(L, 1);
  for (i = 3; i <= n; i++) lua_pushvalue(L, i);
  return lua_pcall(L, n - 2, 1, 0);
}

static int changed(lua_State *L)
{
  int n = lua_gettop(L), status;
  int32_t vmstate = lj_tg_vmstate_load_acq(L2TG(L));
  GCfunc *fn = funcV(L->top - n);
  GCtrace *T;
  assert(isluafunc(fn));
  T = traceref_safe(L2J(L), proto_trace_acq(funcproto(fn)));
  assert(T && trace_runnable_acq(T, T->traceno));
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  status = invoke(L, n);
  assert(status == LUA_OK);
  assert(lua_tonumber(L, -1) == lua_tonumber(L, 2));
  lua_pop(L, 1);
  assert(lj_trace_test_root_entry_publishes() > 0 &&
         lj_trace_test_exit_calls() > 0 &&
         lj_trace_test_first_exitno() < T->nsnap - 1u);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL &&
         lj_tg_in_native_acq(L2TG(L)) == 0 &&
         lj_tg_vmstate_load_acq(L2TG(L)) == vmstate);
  return 0;
}

static int refused(lua_State *L)
{
  GCfunc *fn = funcV(L->base);
  GCtrace *T;
  assert(isluafunc(fn));
  T = traceref_safe(L2J(L), proto_trace_acq(funcproto(fn)));
  assert(!T || !trace_runnable_acq(T, T->traceno));
  assert(lj_trace_test_root_entry_publishes() == 0 &&
         lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL &&
         lj_tg_in_native_acq(L2TG(L)) == 0);
  printf("BITOPS excluded variant remained interpreted; aborts=%u error=%u\n",
         lj_trace_test_abort_count(), (unsigned)lj_trace_test_last_abort_error());
  return 0;
}

static int changed_remat(lua_State *L)
{
  int n = lua_gettop(L);
  GCfunc *fn = funcV(L->top - n);
  GCtrace *T = traceref_safe(L2J(L), proto_trace_acq(funcproto(fn)));
  SnapShot *sp;
  MSize j;
  unsigned remat = 0;
  changed(L);
  sp = &T->snap[lj_trace_test_first_exitno()];
  for (j = 0; j < sp->nent; j++) {
    IRRef r = snap_ref(T->snapmap[sp->mapofs+j]), ren;
    IRIns p;
    RegSP rs;
    if (irref_isk(r)) continue;
    p = T->ir[r]; rs = p.prev;
    for (ren = T->nins; ren-- > REF_FIRST;) {
      IRIns q = T->ir[ren];
      if (q.o != IR_RENAME) break;
      if (q.op1 == r && q.op2 <= lj_trace_test_first_exitno()) rs = q.prev;
    }
    if (p.o == IR_CONV && p.t.irt == IRT_NUM &&
        p.op2 == IRCONV_NUM_INT && ra_noreg(regsp_reg(rs)) &&
        !ra_hasspill(regsp_spill(rs))) remat++;
  }
  assert(remat > 0);
  printf("BITOPS native guard exit restored %u snapshot-only NUM_INT values\n", remat);
  return 0;
}

typedef enum BitopRequest {
  BITOP_PROFILE,
  BITOP_STOPREQ
} BitopRequest;

typedef struct BitopPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  BitopRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} BitopPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ | TGF_STOPREQ_FRESH));
}

static void *publish_bitop_request(void *arg)
{
  BitopPublisher *p = (BitopPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
        LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&p->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&p->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(p->g) == p->epoch);
  assert(lj_tg_hs_epoch_ack_acq(p->tg) == p->epoch);
  assert(gc2_hs_pending_acq(p->g) == 0);
  assert(lj_tg_reqmask_acq(p->tg) == 0 && lj_tg_poll_acq(p->tg) == 0);
  if (lj_tg_load_jit_base(p->tg) != NULL)
    la_store32_rel(&p->saw_jit_base, 1);
  assert(la_load32_acq(&p->saw_jit_base) == 1);
  if (p->request == BITOP_PROFILE) {
    lj_tg_profile_request_rel(p->tg, 1);
  } else {
    assert(p->request == BITOP_STOPREQ);
    gc2_hs_actions_rel(p->g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(p->g, 1);
    gc2_hs_epoch_rel(p->g, p->epoch + 1u);
    lj_tg_reqmask_rel(p->tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(p->tg, 1);
  }
  la_store32_rel(&p->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void bitop_native_idle(lua_State *L, int32_t vmstate)
{
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(lj_tg_vmstate_load_acq(L2TG(L)) == vmstate);
}

static void bitop_equal_result(lua_State *L)
{
  assert(lua_tonumber(L, -1) == lua_tonumber(L, 2));
  lua_pop(L, 1);
}

static int lifecycle(lua_State *L)
{
  int n = lua_gettop(L), status;
  GCfunc *fn = funcV(L->top - n);
  GCtrace *T = traceref_safe(L2J(L), proto_trace_acq(funcproto(fn)));
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  int32_t vmstate = lj_tg_vmstate_load_acq(tg);
  IRRef ref, loop = 0;
  SnapNo s, loopsnap = (SnapNo)~0u, finalexit;
  BitopPublisher p;
  pthread_t worker;
  assert(T && trace_runnable_acq(T, T->traceno));
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].o == IR_LOOP) loop = ref;
  for (s = 0; s < T->nsnap; s++)
    if (T->snap[s].ref == loop) loopsnap = s;
  assert(loop && loopsnap != (SnapNo)~0u);
  finalexit = T->nsnap - 1u;

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  p = (BitopPublisher){g, tg, epoch, BITOP_PROFILE, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_bitop_request, &p) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0 && status == LUA_OK);
  bitop_equal_result(L);
  assert(p.published && lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_exit_calls() == 2 &&
         lj_trace_test_first_exitno() == loopsnap);
  assert(lj_trace_test_last_exitno() == finalexit &&
         lj_tg_profile_request_acq(tg) == 0);
  bitop_native_idle(L, vmstate);

  reset(L);
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  p = (BitopPublisher){g, tg, epoch, BITOP_STOPREQ, 0, 0, 0};
  assert(pthread_create(&worker, NULL, publish_bitop_request, &p) == 0);
  status = invoke(L, n);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown"));
  lua_pop(L, 1);
  assert(p.published && lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_exit_calls() == 1 &&
         lj_trace_test_last_exitno() == loopsnap);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u &&
         lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u);
  assert(!gc2_hs_pending_acq(g) && !lj_tg_reqmask_acq(tg) &&
         !lj_tg_poll_acq(tg));
  assert(lj_tg_flags_acq(tg) & TGF_STOPREQ);
  bitop_native_idle(L, vmstate);
  clear_stopreq(tg);

  reset(L);
  assert(invoke(L, n) == LUA_OK);
  bitop_equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1 &&
         lj_trace_test_last_exitno() == finalexit);
  bitop_native_idle(L, vmstate);
  lua_gc(L, LUA_GCCOLLECT, 0);
  reset(L);
  assert(invoke(L, n) == LUA_OK);
  bitop_equal_result(L);
  assert(lj_trace_test_root_entry_publishes() == 1 &&
         lj_trace_test_exit_calls() == 1);
  bitop_native_idle(L, vmstate);
  puts("BITOPS lifecycle: profile exit, STOPREQ unwind, resumed native entry, GC passed");
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  setvbuf(stdout, NULL, _IONBF, 0);
  assert(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, reset); lua_setglobal(L, "probe_reset");
  lua_pushcfunction(L, check); lua_setglobal(L, "probe_check");
  lua_pushcfunction(L, changed); lua_setglobal(L, "probe_changed");
  lua_pushcfunction(L, changed_remat); lua_setglobal(L, "probe_changed_remat");
  lua_pushcfunction(L, lifecycle); lua_setglobal(L, "probe_lifecycle");
  lua_pushcfunction(L, fused_check); lua_setglobal(L, "probe_fused");
  lua_pushcfunction(L, refused); lua_setglobal(L, "probe_refused");
  assert(argc == 1 || argc == 2);
  if (luaL_dofile(L, argc == 2 ? argv[1] :
                                "tests/t-arm64-jit-bitops.lua")) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    return 1;
  }
  {
    uint64_t required = (UINT64_C(1) <<
        (sizeof(bitops)/sizeof(bitops[0]))) - 1u;
    required &= ~(UINT64_C(1) << 9);  /* BROL remains intentionally closed. */
    assert((covered_ops & required) == required);
    printf("BITOPS totals machine_mask=%u fused=%u rematerialized_snapshots=%u integer_spills=%u cross_loop_rejections=%u\n",
           machine_ops, fused_integers, snapshot_conversions, integer_spills,
           cross_loop_fusions);
    assert(machine_ops == 1023u && fused_integers && integer_spills &&
           snapshot_conversions && cross_loop_fusions);
  }
  lua_close(L);
  return 0;
}

#else
int main(void)
{
  puts("t-arm64-jit-bitops SKIP: requires experimental Apple ARM64 JIT helpers");
  return 0;
}
#endif
