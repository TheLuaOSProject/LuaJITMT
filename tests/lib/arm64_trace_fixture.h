/* Mechanical copying of a live root for independent admission mutation tests.
** Never mutate a published trace or dereference below its biased nk bound. */
#ifndef TESTS_ARM64_TRACE_FIXTURE_H
#define TESTS_ARM64_TRACE_FIXTURE_H
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_jit.h"
#include "lj_trace.h"

typedef struct Arm64TraceFixture {
  GCtrace *owner, semantic;
  jit_State *J;
  IRIns *ir, *good, *postra;
  SnapShot *snaps, *good_snaps;
  SnapEntry *map, *postmap;
  LJArm64PostRAView view;
  IRRef nins, semantic_nins;
  size_t irbytes, snapbytes;
} Arm64TraceFixture;

static void arm64_fixture_init_trace_impl(Arm64TraceFixture *f, lua_State *L,
                                          const char *clone_global,
                                          TraceNo traceno,
                                          const BCIns *external_pc)
{
  GCproto *pt, *original;
  IRRef ref, loop = 0;
  SnapNo s;
  const BCIns *pc;
  size_t mapbytes;
  memset(f, 0, sizeof(*f));
  f->owner = traceref_safe(L2J(L), traceno);
  assert(f->owner && trace_runnable_acq(f->owner, traceno));
  original = trace_startpt_acq(f->owner);
  lua_getglobal(L, clone_global);
  assert(lua_isfunction(L, -1) && isluafunc(funcV(L->top-1)));
  pt = funcproto(funcV(L->top-1));
  /* Leave the cloned function on the Lua stack throughout the fixture. */
  pc = proto_bc(pt) + (trace_startpc_acq(f->owner) - proto_bc(original));
  assert(*pc == trace_startins_acq(f->owner));
  f->nins = trace_nins_acq(f->owner);
  f->irbytes = (size_t)f->nins * sizeof(IRIns);
  f->snapbytes = trace_nsnap_acq(f->owner) * sizeof(SnapShot);
  mapbytes = trace_nsnapmap_acq(f->owner) * sizeof(SnapEntry);
  f->J = (jit_State *)calloc(1, sizeof(jit_State));
  f->ir = (IRIns *)calloc(f->nins, sizeof(IRIns));
  f->good = (IRIns *)calloc(f->nins, sizeof(IRIns));
  f->postra = (IRIns *)calloc(f->nins, sizeof(IRIns));
  f->snaps = (SnapShot *)malloc(f->snapbytes);
  f->good_snaps = (SnapShot *)malloc(f->snapbytes);
  f->map = (SnapEntry *)malloc(mapbytes);
  f->postmap = (SnapEntry *)malloc(mapbytes);
  assert(f->J && f->ir && f->good && f->postra && f->snaps &&
         f->good_snaps && f->map && f->postmap);
  memcpy(f->good + f->owner->nk, trace_ir_acq(f->owner) + f->owner->nk,
         (f->nins - f->owner->nk) * sizeof(IRIns));
  memcpy(f->postra, f->good, f->irbytes);
  memcpy(f->snaps, trace_snap_acq(f->owner), f->snapbytes);
  memcpy(f->good_snaps, f->snaps, f->snapbytes);
  memcpy(f->map, trace_snapmap_acq(f->owner), mapbytes);
  memcpy(f->postmap, f->map, mapbytes);
  f->semantic_nins = f->nins;
  while (f->semantic_nins > REF_FIRST &&
         (f->good[f->semantic_nins-1].o == IR_RENAME ||
          f->good[f->semantic_nins-1].o == IR_NOP))
    f->semantic_nins--;
  for (ref = REF_FIRST; ref < f->semantic_nins; ref++) {
    if (f->good[ref].o == IR_LOOP) {
      loop = ref;
      f->good[ref].op12 = 0;
    } else if (f->good[ref].o == IR_XPOLL) {
      f->good[ref].op1 = 1; f->good[ref].op2 = 0;
    }
  }
  for (s = 0; s < f->owner->nsnap; s++) {
    SnapShot *snap = &f->snaps[s];
    uint64_t footer;
    const BCIns *oldpc, *newpc;
    uintptr_t oldaddr, original_lo, original_hi;
    memcpy(&footer, &f->map[snap->mapofs+snap->nent], sizeof(footer));
    oldpc = (const BCIns *)(uintptr_t)(footer >> 8);
    oldaddr = (uintptr_t)(const void *)oldpc;
    original_lo = (uintptr_t)(const void *)proto_bc(original);
    original_hi = original_lo+(uintptr_t)original->sizebc*sizeof(BCIns);
    if (oldaddr >= original_lo && oldaddr < original_hi) {
      newpc = proto_bc(pt) + (oldpc-proto_bc(original));
      footer = ((uint64_t)(uintptr_t)newpc << 8) | (uint8_t)footer;
      memcpy(&f->map[snap->mapofs+snap->nent], &footer, sizeof(footer));
    } else {
      assert(external_pc != NULL && oldaddr ==
	     (uintptr_t)(const void *)external_pc);
    }
  }
  f->semantic = *f->owner;
  f->semantic.ir = f->ir; f->semantic.snap = f->snaps;
  f->semantic.snapmap = f->map; f->semantic.nins = f->semantic_nins;
  trace_startpt_rel(&f->semantic, pt);
  setmref(f->semantic.startpc, pc);
  f->J->L = L; f->J->pt = pt; f->J->startpc = pc;
  f->J->baseslot = 1+LJ_FR2; f->J->loopref = loop;
  f->view.ir = f->postra; f->view.owner = f->owner;
  f->view.snap = f->snaps; f->view.snapmap = f->postmap;
  f->view.proto_bc = proto_bc(original); f->view.nins = f->nins;
  f->view.nk = f->owner->nk; f->view.nsnap = f->owner->nsnap;
  f->view.nsnapmap = f->owner->nsnapmap;
  f->view.spadjust = f->owner->spadjust;
  f->view.proto_sizebc = original->sizebc;
  f->view.proto_numparams = original->numparams;
  f->view.root_topslot = f->owner->topslot;
  f->view.startins = f->owner->startins;
  memcpy(f->ir, f->good, f->irbytes);
}

static void arm64_fixture_init_trace(Arm64TraceFixture *f, lua_State *L,
                                     const char *clone_global,
                                     TraceNo traceno)
{
  arm64_fixture_init_trace_impl(f, L, clone_global, traceno, NULL);
}

/* CALLXS snapshots may contain the exact FFI __call continuation PC. Keep it
** unchanged while remapping only the cloned root's own bytecode positions. */
static void arm64_fixture_init_trace_callxs_metapc(Arm64TraceFixture *f,
                                                   lua_State *L,
                                                   const char *clone_global,
                                                   TraceNo traceno,
                                                   const BCIns *meta_pc)
{
  GCproto *original, *clone;
  uintptr_t lo, hi;
  SnapNo s;
  assert(meta_pc != NULL);
  arm64_fixture_init_trace_impl(f, L, clone_global, traceno, meta_pc);
  original = trace_startpt_acq(f->owner);
  clone = trace_startpt_acq(&f->semantic);
  lo = (uintptr_t)(const void *)proto_bc(original);
  hi = lo+(uintptr_t)original->sizebc*sizeof(BCIns);
  /* CALLXS frame snapshots also carry a KNUM PC constant used to bind the
  ** FFI __call return continuation. Match the private proto clone just as
  ** the footer PCs above do; leave the separately validated meta_pc intact. */
  for (s = 0; s < f->owner->nsnap; s++) {
    SnapShot *snap = &f->snaps[s];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), i;
    for (i = 0; i < nent; i++) {
      SnapEntry entry = snapentry_acq(&f->map[ofs+i]);
      IRRef ref = snap_ref(entry);
      if ((entry & 0x00ff0000u) == SNAP_FRAME && ref >= f->owner->nk &&
          ref+1u < REF_TRUE && f->good[ref].o == IR_KNUM &&
          f->good[ref].t.irt == IRT_NUM && f->good[ref].op12 == 0) {
        uint64_t bits = ir_load_acq(&f->good[ref+1u]).tv.u64;
        uintptr_t pc = (uintptr_t)bits;
        if (pc >= lo && pc < hi &&
            (pc-lo) % sizeof(BCIns) == 0) {
          pc = (uintptr_t)(const void *)proto_bc(clone) + (pc-lo);
          f->good[ref+1u].tv.u64 = (uint64_t)pc;
        }
      }
    }
  }
  memcpy(f->ir+f->owner->nk, f->good+f->owner->nk,
         (f->nins-f->owner->nk)*sizeof(IRIns));
  memcpy(f->postra+f->owner->nk, f->good+f->owner->nk,
         (f->nins-f->owner->nk)*sizeof(IRIns));
}

static void arm64_fixture_init(Arm64TraceFixture *f, lua_State *L,
                               const char *clone_global)
{
  arm64_fixture_init_trace(f, L, clone_global, 1);
}

static void arm64_fixture_reset(Arm64TraceFixture *f)
{
  memcpy(f->ir, f->good, f->irbytes);
  memcpy(f->postra + f->owner->nk, trace_ir_acq(f->owner) + f->owner->nk,
         (f->nins-f->owner->nk)*sizeof(IRIns));
  memcpy(f->snaps, f->good_snaps, f->snapbytes);
}

static void arm64_fixture_free(Arm64TraceFixture *f)
{
  free(f->J); free(f->ir); free(f->good); free(f->postra);
  free(f->snaps); free(f->good_snaps); free(f->map); free(f->postmap);
}
#endif
