#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "rooted cell admission requires experimental ARM64"
#endif

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &reject);
}

static int allocated(Arm64TraceFixture *f)
{
  IRRef nins = 0;
  return lj_asm_arm64_postra_admit(&f->view, &nins);
}

static IRRef find_call(const Arm64TraceFixture *f, int get)
{
  IRRef r;
  IRCallID id = get ? IRCALL_lj_func_getcell_rooted_try :
                     IRCALL_lj_func_storecell_rooted_try;
  for (r = REF_FIRST; r < f->semantic_nins; r++)
    if (f->ir[r].o == IR_CALLS && f->ir[r].op2 == id)
      return r;
  assert(!"missing rooted cell call");
  return 0;
}

static SnapNo find_snapshot(const SnapShot *snaps, MSize nsnap, IRRef ref)
{
  SnapNo s;
  for (s = 0; s < nsnap; s++)
    if (snap_ref_acq(&snaps[s]) == ref)
      return s;
  assert(!"missing rooted cell snapshot");
  return (SnapNo)~0u;
}

static SnapNo find_latest_snapshot(const SnapShot *snaps, MSize nsnap,
                                   IRRef ref)
{
  SnapNo s, found = (SnapNo)~0u;
  for (s = 0; s < nsnap; s++)
    if (snap_ref_acq(&snaps[s]) <= ref)
      found = s;
  assert(found != (SnapNo)~0u);
  return found;
}

static void offset_snapshot_pc(SnapEntry *map, const SnapShot *snap,
                               ptrdiff_t bytecode_delta)
{
  SnapEntry *footer = &map[snap_mapofs_acq(snap) + snap_nent_acq(snap)];
  uint64_t packed;
  uintptr_t pc;
  memcpy(&packed, footer, sizeof(packed));
  pc = (uintptr_t)(packed >> 8);
  pc += (uintptr_t)bytecode_delta * sizeof(BCIns);
  packed = ((uint64_t)pc << 8) | (uint8_t)packed;
  memcpy(footer, &packed, sizeof(packed));
}

static IRRef find_cell_sload(const Arm64TraceFixture *f, BCReg cellslot)
{
  IRRef r, found = 0;
  BCReg stackslot = (BCReg)(f->J->baseslot + cellslot);
  for (r = REF_FIRST; r < f->semantic_nins; r++) {
    IRIns *ir = &f->ir[r];
    if (ir->o == IR_SLOAD && ir->op1 == stackslot &&
        ir->t.irt == (IRT_P32|IRT_GUARD) &&
        ir->op2 == IRSLOAD_TYPECHECK) {
      assert(!found);
      found = r;
    }
  }
  assert(found);
  return found;
}

static int ir_uses_ref(const IRIns *ir, IRRef ref)
{
  uint8_t mode = lj_ir_mode[ir->o];
  return (irm_op1(mode) == IRMref && ir->op1 == ref) ||
         (irm_op2(mode) == IRMref && ir->op2 == ref);
}

static void assert_cell_sload_is_guard_only(const Arm64TraceFixture *f,
                                             IRRef source)
{
  IRRef r;
  assert(f->ir[source].o == IR_SLOAD &&
         f->ir[source].t.irt == (IRT_P32|IRT_GUARD) &&
         f->ir[source].op2 == IRSLOAD_TYPECHECK);
  for (r = REF_FIRST; r < f->semantic_nins; r++)
    if (r != source)
      assert(!ir_uses_ref(&f->ir[r], source));
}

static void assert_cell_sload_postra_guard_only(const Arm64TraceFixture *f,
                                                 IRRef source)
{
  IRRef r;
  IRIns *ir = f->postra;
  assert(ir[source].o == IR_SLOAD &&
         ir[source].t.irt == (IRT_P32|IRT_GUARD) &&
         ir[source].op2 == IRSLOAD_TYPECHECK &&
         ra_noreg(ir[source].r) && !ra_hasspill(ir[source].s));
  for (r = REF_FIRST; r < f->semantic_nins; r++)
    if (r != source)
      assert(!ir_uses_ref(&ir[r], source));
}

static int snapshot_has_slot(const SnapShot *snaps, const SnapEntry *map,
                             MSize nsnap, BCReg slot)
{
  SnapNo s;
  for (s = 0; s < nsnap; s++) {
    MSize i, ofs = snap_mapofs_acq(&snaps[s]);
    MSize nent = snap_nent_acq(&snaps[s]);
    for (i = 0; i < nent; i++)
      if (snap_slot(snapentry_acq(&map[ofs+i])) == slot)
        return 1;
  }
  return 0;
}

static MSize find_value_map_entry(const SnapShot *snaps, const SnapEntry *map,
                                  MSize nsnap, IRRef after)
{
  SnapNo s;
  for (s = 0; s < nsnap; s++) {
    MSize i, ofs = snap_mapofs_acq(&snaps[s]);
    MSize nent = snap_nent_acq(&snaps[s]);
    if (snap_ref_acq(&snaps[s]) <= after) continue;
    for (i = 0; i < nent; i++) {
      SnapEntry entry = snapentry_acq(&map[ofs+i]);
      if (!(entry & SNAP_FRAME) && snap_slot(entry) >= 1u+LJ_FR2)
        return ofs+i;
    }
  }
  assert(!"no value snapshot entry to mutate");
  return 0;
}

static int type_mode(const char *mode)
{
  static const char *const modes[] = {
    "get", "get-forl", "store", "store-forl", "store-integer",
    "store-number", "store-nil", "store-false", "store-true",
    "store-string", "store-table", "store-function", "store-thread",
    "store-userdata", "store-cdata"
  };
  size_t i;
  for (i = 0; i < sizeof(modes)/sizeof(modes[0]); i++)
    if (!strcmp(mode, modes[i])) return 1;
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  SnapShot *viewsnaps, *savedviewsnaps;
  const GCproto *viewproto;
  const BCIns *viewstartpc;
  BCIns *semantic_bc, *owner_bc;
  ptrdiff_t pc_offset;
  BCIns original_semantic_bc, original_owner_bc;
  IRRef call, tail, head, root1, index, cellload, statusk, result = 0;
  IRRef nins;
  SnapNo pre, post = (SnapNo)~0u;
  const BCIns *prepc, *postpc = NULL;
  BCReg cellslot, stackslot, other_slot;
  size_t mapbytes;
  SnapEntry *savedmap, *savedpostmap;
  IRIns saved_index, saved_post_index, saved_status, saved_post_status;
  MSize semantic_framesize, owner_framesize;
  uint32_t semantic_flags2, owner_flags2;
  int get, forl, sem, postra;
  int type;

  assert(L && argc == 3 && type_mode(argv[2]));
  get = !strncmp(argv[2], "get", 3);
  forl = strstr(argv[2], "forl") != NULL;
  type = !strcmp(argv[2], "store-integer") ? 1 :
         !strcmp(argv[2], "store-number") ? 2 :
         !strcmp(argv[2], "store-nil") ? 3 :
         !strcmp(argv[2], "store-false") ? 4 :
         !strcmp(argv[2], "store-true") ? 5 :
         !strcmp(argv[2], "store-string") ? 6 :
         !strcmp(argv[2], "store-table") ? 7 :
         !strcmp(argv[2], "store-function") ? 8 :
         !strcmp(argv[2], "store-thread") ? 9 :
         !strcmp(argv[2], "store-userdata") ? 10 :
         !strcmp(argv[2], "store-cdata") ? 11 : 0;
  luaL_openlibs(L);
  lua_pushboolean(L, 1);
  lua_setglobal(L, "rooted_cell_admission");
  lua_pushstring(L, argv[2]);
  lua_setglobal(L, "rooted_cell_mode");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "rooted cell admission fixture: %s\n", lua_tostring(L, -1));
    abort();
  }

  arm64_fixture_init(&f, L, "rooted_cell_trace_clone");
  f.view.proto = trace_startpt_acq(f.owner);
  f.view.startpc = trace_startpc_acq(f.owner);
  viewproto = f.view.proto;
  viewstartpc = f.view.startpc;
  viewsnaps = (SnapShot *)malloc(f.snapbytes);
  savedviewsnaps = (SnapShot *)malloc(f.snapbytes);
  assert(viewsnaps && savedviewsnaps);
  memcpy(viewsnaps, f.snaps, f.snapbytes);
  memcpy(savedviewsnaps, f.snaps, f.snapbytes);
  f.view.snap = viewsnaps;
  assert(semantic(&f) && allocated(&f));

  call = find_call(&f, get);
  assert(call + (get ? 2u : 1u) < f.semantic_nins);
  tail = f.ir[call].op1;
  assert(tail > REF_FIRST && tail < call && f.ir[tail].o == IR_CARG);
  head = f.ir[tail].op1;
  assert(head > REF_FIRST && head < tail && f.ir[head].o == IR_CARG);
  root1 = f.ir[tail].op2;
  index = f.ir[head].op2;
  assert(f.ir[head].op1 == REF_BASE);
  assert(root1 > REF_FIRST && root1 < tail &&
         f.ir[root1].o == IR_TMPREF && f.ir[root1].t.irt == IRT_PGC &&
         f.ir[root1].op2 == (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) :
                                    IRTMPREF_IN1));
  if (get) assert(f.ir[root1].op1 == REF_NIL);
  assert(f.ir[head].t.irt == IRT_NIL && f.ir[tail].t.irt == IRT_NIL);
  assert(index >= f.semantic.nk && index < REF_TRUE &&
         index < head &&
         f.ir[index].o == IR_KINT && f.ir[index].t.irt == IRT_INT);
  assert(f.postra[head].op2 == index &&
         f.postra[index].o == IR_KINT && f.postra[index].t.irt == IRT_INT);
  assert(f.ir[call].t.irt == (IRT_INT|IRT_GUARD));
  assert(f.ir[call].op2 == (get ? IRCALL_lj_func_getcell_rooted_try :
                                  IRCALL_lj_func_storecell_rooted_try));

  pre = find_latest_snapshot(f.snaps, f.semantic.nsnap, root1);
  assert(snap_ref_acq(&f.snaps[pre]) == root1);
  prepc = snap_pc(&f.map[snap_mapofs_acq(&f.snaps[pre])+
                         snap_nent_acq(&f.snaps[pre])]);
  cellslot = get ? bc_d(*prepc) : bc_a(*prepc);
  stackslot = (BCReg)(f.J->baseslot + cellslot);
  assert(proto_cellops((GCproto *)f.J->pt));
  assert(cellslot < f.J->pt->framesize &&
         ((get && bc_op(*prepc) == BC_CGET) ||
          (!get && bc_op(*prepc) == BC_CSET)));
  assert((uint32_t)f.ir[index].i == (uint32_t)cellslot);
  assert((uint32_t)f.postra[index].i == (uint32_t)cellslot);
  cellload = find_cell_sload(&f, cellslot);
  assert_cell_sload_is_guard_only(&f, cellload);
  assert_cell_sload_postra_guard_only(&f, cellload);
  assert(!snapshot_has_slot(f.snaps, f.map, f.semantic.nsnap, stackslot));
  assert(!snapshot_has_slot(viewsnaps, f.postmap, f.view.nsnap, stackslot));

  if (get) {
    result = call+2u;
    assert(f.ir[call+1u].o == IR_NE &&
           f.ir[call+1u].t.irt == (IRT_INT|IRT_GUARD));
    statusk = f.ir[call+1u].op1 == call ? f.ir[call+1u].op2 :
                                           f.ir[call+1u].op1;
    assert(statusk >= f.semantic.nk && statusk < REF_TRUE &&
           f.ir[statusk].o == IR_KINT && f.ir[statusk].i == 0);
    assert(f.ir[result].o == IR_VLOAD && f.ir[result].op1 == root1 &&
           f.ir[result].op2 == 0 && irt_isguard(f.ir[result].t));
  } else {
    result = 0;
    assert(f.ir[call+1u].o == IR_NE &&
           f.ir[call+1u].t.irt == (IRT_INT|IRT_GUARD));
    statusk = f.ir[call+1u].op1 == call ? f.ir[call+1u].op2 :
                                           f.ir[call+1u].op1;
    assert(statusk >= f.semantic.nk && statusk < REF_TRUE &&
           f.ir[statusk].o == IR_KINT && f.ir[statusk].i == 0);
    post = find_snapshot(f.snaps, f.semantic.nsnap, call+2u);
    assert(snap_ref_acq(&f.snaps[post]) == call+2u &&
           snap_count_acq(&f.snaps[post]) == SNAPCOUNT_DONE);
    postpc = snap_pc(&f.map[snap_mapofs_acq(&f.snaps[post])+
                            snap_nent_acq(&f.snaps[post])]);
    assert(postpc == prepc+1);
  }
  if (get) post = find_snapshot(f.snaps, f.semantic.nsnap, call+2u);
  assert((!forl && bc_op(f.view.startins) == BC_LOOP) ||
         (forl && bc_op(f.view.startins) == BC_FORL));
  assert(f.view.proto == viewproto && f.view.startpc == viewstartpc);
  if (!get) {
    IRRef rhs = f.ir[root1].op1;
    assert(rhs != REF_BASE && rhs != cellload);
    if (type == 3) assert(rhs == REF_NIL);
    if (type == 4) assert(rhs == REF_FALSE);
    if (type == 5) assert(rhs == REF_TRUE);
    if (type == 1 || type == 2 || type == 6 || type == 7 || type == 8 ||
        type == 9 || type == 10 || type == 11)
      assert(rhs != REF_NIL && rhs != REF_FALSE && rhs != REF_TRUE);
    if (type == 1) assert(irt_type(f.ir[rhs].t) == IRT_INT);
    if (type == 2) assert(irt_type(f.ir[rhs].t) == IRT_NUM);
    if (type == 6) assert(irt_type(f.ir[rhs].t) == IRT_STR);
    if (type == 7) assert(irt_type(f.ir[rhs].t) == IRT_TAB);
    if (type == 8) assert(irt_type(f.ir[rhs].t) == IRT_FUNC);
    if (type == 9) assert(irt_type(f.ir[rhs].t) == IRT_THREAD);
    if (type == 10) assert(irt_type(f.ir[rhs].t) == IRT_UDATA);
    if (type == 11) assert(irt_type(f.ir[rhs].t) == IRT_CDATA);
  }

  mapbytes = f.semantic.nsnapmap * sizeof(SnapEntry);
  savedmap = (SnapEntry *)malloc(mapbytes);
  savedpostmap = (SnapEntry *)malloc(mapbytes);
  assert(savedmap && savedpostmap);
  memcpy(savedmap, f.map, mapbytes);
  memcpy(savedpostmap, f.postmap, mapbytes);
  semantic_bc = (BCIns *)(void *)prepc;
  pc_offset = prepc - proto_bc(f.J->pt);
  owner_bc = (BCIns *)(void *)(f.view.proto_bc + pc_offset);
  original_semantic_bc = *semantic_bc;
  original_owner_bc = *owner_bc;
  semantic_framesize = f.J->pt->framesize;
  owner_framesize = ((GCproto *)f.view.proto)->framesize;
  semantic_flags2 = f.J->pt->flags2;
  owner_flags2 = ((GCproto *)f.view.proto)->flags2;
  saved_index = f.ir[index]; saved_post_index = f.postra[index];
  saved_status = f.ir[statusk]; saved_post_status = f.postra[statusk];
  other_slot = cellslot+1u < f.J->pt->framesize ? cellslot+1u : cellslot-1u;
  assert(other_slot != cellslot && other_slot < f.J->pt->framesize);

#define RESET() do { \
  arm64_fixture_reset(&f); \
  f.view.proto = viewproto; f.view.startpc = viewstartpc; \
  f.semantic.startins = f.owner->startins; f.view.startins = f.owner->startins; \
  memcpy(viewsnaps, savedviewsnaps, f.snapbytes); \
  memcpy(f.map, savedmap, mapbytes); \
  memcpy(f.postmap, savedpostmap, mapbytes); \
  *semantic_bc = original_semantic_bc; *owner_bc = original_owner_bc; \
  f.J->pt->framesize = semantic_framesize; \
  ((GCproto *)f.view.proto)->framesize = owner_framesize; \
  f.J->pt->flags2 = semantic_flags2; \
  ((GCproto *)f.view.proto)->flags2 = owner_flags2; \
  f.ir[index] = saved_index; f.postra[index] = saved_post_index; \
  f.ir[statusk] = saved_status; f.postra[statusk] = saved_post_status; \
  assert(semantic(&f) && allocated(&f)); \
} while (0)
#define SEM_REJECT(change) do { RESET(); change; \
  sem = semantic(&f); postra = allocated(&f); \
  if (sem || !postra) { fprintf(stderr, "cell semantic mutation accepted line %d\n", __LINE__); abort(); } \
} while (0)
#define POST_REJECT(change) do { RESET(); change; \
  sem = semantic(&f); postra = allocated(&f); \
  if (!sem || postra) { fprintf(stderr, "cell postRA mutation accepted line %d\n", __LINE__); abort(); } \
} while (0)

  /* Exact source opcode and selected raw-cell slot are independent evidence. */
  {
    BCIns wrongop = (original_semantic_bc & ~(BCIns)0xffu) |
                    (BCIns)(get ? BC_CSET : BC_CGET);
    BCIns wrongslot = get ?
      ((original_semantic_bc & 0x0000ffffu) | ((BCIns)other_slot << 16)) :
      ((original_semantic_bc & ~0x0000ff00u) |
       ((BCIns)other_slot << 8));
    SEM_REJECT(*semantic_bc = wrongop);
    SEM_REJECT(*semantic_bc = wrongslot);
    POST_REJECT(*owner_bc = wrongop);
    POST_REJECT(*owner_bc = wrongslot);
    if (!get) {
      BCIns wrongrhs = (original_semantic_bc & 0x0000ffffu) |
                       ((BCIns)other_slot << 16);
      SEM_REJECT(*semantic_bc = wrongrhs);
      POST_REJECT(*owner_bc = wrongrhs);
    }
  }
  SEM_REJECT(f.ir[call].op2 = get ? IRCALL_lj_func_storecell_rooted_try :
                                    IRCALL_lj_func_getcell_rooted_try);
  POST_REJECT(f.postra[call].op2 = get ? IRCALL_lj_func_storecell_rooted_try :
                                          IRCALL_lj_func_getcell_rooted_try);
  SEM_REJECT(f.ir[head].op1 = cellload);  /* Never pass the transient raw cell pointer. */
  POST_REJECT(f.postra[head].op1 = cellload);
  SEM_REJECT(f.ir[head].op1 = root1);
  POST_REJECT(f.postra[head].op1 = root1);
  SEM_REJECT(f.ir[head].op2 = f.ir[index].i ? REF_NIL : REF_FALSE);
  POST_REJECT(f.postra[head].op2 = f.ir[index].i ? REF_NIL : REF_FALSE);
  SEM_REJECT(f.ir[index].i = (int32_t)other_slot);
  POST_REJECT(f.postra[index].i = (int32_t)other_slot);
  SEM_REJECT(f.ir[tail].op2 = REF_NIL);
  POST_REJECT(f.postra[tail].op2 = REF_NIL);
  SEM_REJECT(f.ir[root1].op2 = get ? IRTMPREF_IN1 :
                                      (IRTMPREF_IN1|IRTMPREF_OUT1));
  POST_REJECT(f.postra[root1].op2 = get ? IRTMPREF_IN1 :
                                          (IRTMPREF_IN1|IRTMPREF_OUT1));
  SEM_REJECT(f.ir[root1].op1 = cellload);
  POST_REJECT(f.postra[root1].op1 = cellload);
  SEM_REJECT(f.ir[cellload].op1++);
  POST_REJECT(f.postra[cellload].op1++);
  SEM_REJECT(f.ir[cellload].op2 = IRSLOAD_PARENT);
  POST_REJECT(f.postra[cellload].op2 = IRSLOAD_PARENT);
  SEM_REJECT(f.ir[cellload].t.irt = IRT_PGC|IRT_GUARD);
  POST_REJECT(f.postra[cellload].t.irt = IRT_PGC|IRT_GUARD);
  SEM_REJECT(f.ir[cellload].t.irt &= (IRType)~IRT_GUARD);
  POST_REJECT(f.postra[cellload].t.irt &= (IRType)~IRT_GUARD);
  SEM_REJECT(f.ir[call+1u].o = IR_EQ);
  POST_REJECT(f.postra[call+1u].o = IR_EQ);
  SEM_REJECT(f.ir[call+1u].o = IR_NOP);
  POST_REJECT(f.postra[call+1u].o = IR_NOP);
  SEM_REJECT(f.ir[statusk].i = 1);
  POST_REJECT(f.postra[statusk].i = 1);
  if (get) {
    SEM_REJECT(f.ir[result].op1 = cellload);
    POST_REJECT(f.postra[result].op1 = cellload);
    SEM_REJECT(f.ir[result].t.irt &= (IRType)~IRT_GUARD);
    POST_REJECT(f.postra[result].t.irt &= (IRType)~IRT_GUARD);
  }
  SEM_REJECT(f.snaps[pre].ref = call);
  POST_REJECT(viewsnaps[pre].ref = call);
  SEM_REJECT(f.snaps[pre].mapofs = (uint32_t)f.semantic.nsnapmap);
  POST_REJECT(viewsnaps[pre].mapofs = (uint32_t)f.view.nsnapmap);
  SEM_REJECT(f.snaps[pre].nent = 255);
  POST_REJECT(viewsnaps[pre].nent = 255);
  SEM_REJECT(f.snaps[post].mapofs = (uint32_t)f.semantic.nsnapmap);
  POST_REJECT(viewsnaps[post].mapofs = (uint32_t)f.view.nsnapmap);
  SEM_REJECT(f.snaps[post].nent = 255);
  POST_REJECT(viewsnaps[post].nent = 255);
  SEM_REJECT(offset_snapshot_pc(f.map, &f.snaps[pre], 1));
  POST_REJECT(offset_snapshot_pc(f.postmap, &viewsnaps[pre], 1));
  if (!get) {
    SEM_REJECT(f.snaps[post].count = 0);
    POST_REJECT(viewsnaps[post].count = 0);
    SEM_REJECT(f.snaps[post].ref = call+1u);
    POST_REJECT(viewsnaps[post].ref = call+1u);
    SEM_REJECT(offset_snapshot_pc(f.map, &f.snaps[post], 1));
    POST_REJECT(offset_snapshot_pc(f.postmap, &viewsnaps[post], 1));
  }
  /* The bytecode slot must also fit the owning proto's real frame. */
  SEM_REJECT(f.J->pt->framesize = (uint8_t)cellslot);
  POST_REJECT(((GCproto *)f.view.proto)->framesize = (uint8_t)cellslot);
  SEM_REJECT(f.J->pt->flags2 &= (uint32_t)~PROTO2_CELLOPS);
  POST_REJECT(((GCproto *)f.view.proto)->flags2 &= (uint32_t)~PROTO2_CELLOPS);
  /* Neither visible restoration over the cell slot nor a raw cell value
  ** is permitted, including entries marked NORESTORE. Choose snapshots after
  ** the cell guard so rejection cannot be explained by a forward IR ref. */
  {
    MSize m = find_value_map_entry(f.snaps, f.map, f.semantic.nsnap,
                                  cellload);
    SEM_REJECT(f.map[m] = SNAP(stackslot, 0, snap_ref(f.map[m])));
    SEM_REJECT(f.map[m] = SNAP(stackslot, SNAP_NORESTORE,
                              snap_ref(f.map[m])));
    SEM_REJECT(f.map[m] = SNAP(snap_slot(f.map[m]), 0, cellload));
    SEM_REJECT(f.map[m] = SNAP(snap_slot(f.map[m]), SNAP_NORESTORE,
                              cellload));
    m = find_value_map_entry(viewsnaps, f.postmap, f.view.nsnap, cellload);
    POST_REJECT(f.postmap[m] = SNAP(stackslot, 0, snap_ref(f.postmap[m])));
    POST_REJECT(f.postmap[m] = SNAP(stackslot, SNAP_NORESTORE,
                                   snap_ref(f.postmap[m])));
    POST_REJECT(f.postmap[m] = SNAP(snap_slot(f.postmap[m]), 0, cellload));
    POST_REJECT(f.postmap[m] = SNAP(snap_slot(f.postmap[m]), SNAP_NORESTORE,
                                   cellload));
  }
  if (!get) {
    IRRef rhs = f.ir[root1].op1;
    IRRef wrong_rhs = rhs == REF_NIL ? REF_TRUE : REF_NIL;
    SEM_REJECT(f.ir[root1].op1 = wrong_rhs);
    POST_REJECT(f.postra[root1].op1 = wrong_rhs);
  }
  /* A helper-bearing trace is root-only; changing it to a side entry must
  ** fail both independent public admission paths. */
  SEM_REJECT(f.semantic.startins = BCINS_AD(BC_JMP, 0, 0));
  POST_REJECT(f.view.startins = BCINS_AD(BC_JMP, 0, 0));
  /* The guard-only cell source has no allocator state to accept or spill. */
  POST_REJECT(f.postra[cellload].r = RID_X0);
  POST_REJECT(f.postra[cellload].s = SPS_LIMIT-1);

  RESET();
  assert(semantic(&f) && allocated(&f));
  nins = f.view.nins;
  assert(nins == f.nins);
  printf("rooted cell %s admission: call=%u source=%u slot=%u\n",
         argv[2], (unsigned)call, (unsigned)cellload, (unsigned)cellslot);
  arm64_fixture_free(&f);
  free(savedviewsnaps); free(viewsnaps); free(savedmap); free(savedpostmap);
  lua_close(L);
  puts("t-arm64-jit-rooted-cell-admission OK");
  return 0;
}
