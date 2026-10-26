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
#include "lj_ircall.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "rooted upvalue admission requires experimental ARM64"
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
  IRCallID id = get ? IRCALL_lj_func_getuv_rooted_try :
			IRCALL_lj_func_storeuv_rooted_try;
  for (r = REF_FIRST; r < f->semantic_nins; r++)
    if (f->ir[r].o == IR_CALLS && f->ir[r].op2 == id)
      return r;
  assert(!"missing rooted upvalue call");
  return 0;
}

static SnapNo find_snapshot(const Arm64TraceFixture *f, IRRef ref)
{
  SnapNo s;
  for (s = 0; s < f->semantic.nsnap; s++)
    if (f->snaps[s].ref == ref)
      return s;
  assert(!"missing rooted upvalue snapshot");
  return (SnapNo)~0u;
}

static SnapNo find_latest_snapshot(const Arm64TraceFixture *f, IRRef ref)
{
  SnapNo s, found = (SnapNo)~0u;
  for (s = 0; s < f->semantic.nsnap; s++)
    if (f->snaps[s].ref <= ref)
      found = s;
  assert(found != (SnapNo)~0u);
  return found;
}

static void set_test_kint(IRIns *ir, int32_t value)
{
  memset(ir, 0, sizeof(*ir));
  ir->i = value;
  ir->t.irt = IRT_INT;
  ir->o = IR_KINT;
}

static void force_double_literal_proto(GCproto *pt, lua_Number value)
{
  MSize pc;
  unsigned found = 0;
  for (pc = 0; pc < pt->sizebc; pc++) {
    BCIns ins = proto_bc(pt)[pc];
    if (bc_op(ins) == BC_USETN) {
      MSize kn = bc_d(ins);
      assert(kn < pt->sizekn);
      setnumV((TValue *)(void *)proto_knumtv(pt, kn), value);
      found++;
    }
  }
  assert(found == 1);
}

static int force_double_literal(lua_State *L)
{
  assert(L->top > L->base && tvisfunc(L->base) &&
         isluafunc(funcV(L->base)));
  force_double_literal_proto(funcproto(funcV(L->base)), luaL_checknumber(L, 3));
  lua_pushboolean(L, 1);
  return 1;
}

static void offset_snapshot_pc(SnapEntry *map, const SnapShot *snap,
			       ptrdiff_t bytecode_delta)
{
  SnapEntry *footer = &map[snap->mapofs + snap->nent];
  uint64_t packed;
  uintptr_t pc;
  memcpy(&packed, footer, sizeof(packed));
  pc = (uintptr_t)(packed >> 8);
  pc += (uintptr_t)bytecode_delta * sizeof(BCIns);
  packed = ((uint64_t)pc << 8) | (uint8_t)packed;
  memcpy(footer, &packed, sizeof(packed));
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  SnapShot *viewsnaps, *savedviewsnaps;
  const GCproto *viewproto;
  const BCIns *viewstartpc;
  int get, forl;
  size_t mapbytes;
  SnapEntry *savedmap, *savedpostmap;
  IRRef call, tail, head, root1, root2 = 0, index, source, guard, statusk;
  IRRef result = 0, wrong_index = 0;
  SnapNo pre, post;
  const BCIns *prepc, *postpc;
  uint32_t uv;
  int sem, postra;
  int literal, literal_knum, literal_kint;

  assert(L && argc == 3);
  get = !strcmp(argv[2], "get") || !strcmp(argv[2], "get-forl");
  forl = !strcmp(argv[2], "get-forl") || !strcmp(argv[2], "store-forl");
  literal = strncmp(argv[2], "literal-", 8) == 0;
  literal_kint = !strcmp(argv[2], "literal-integer") ||
                 !strcmp(argv[2], "literal-zero");
  literal_knum = !strcmp(argv[2], "literal-number") ||
                 !strcmp(argv[2], "literal-negzero") ||
                 !strcmp(argv[2], "literal-double-integer");
  assert(get || !strcmp(argv[2], "store") || forl ||
	 !strcmp(argv[2], "literal-string") ||
	 !strcmp(argv[2], "literal-number") ||
	 !strcmp(argv[2], "literal-double-integer") ||
	 !strcmp(argv[2], "literal-integer") ||
	 !strcmp(argv[2], "literal-zero") ||
	 !strcmp(argv[2], "literal-negzero") ||
	 !strcmp(argv[2], "literal-nil") ||
	 !strcmp(argv[2], "literal-false") ||
	 !strcmp(argv[2], "literal-true"));
  luaL_openlibs(L);
  lua_pushcfunction(L, force_double_literal);
  lua_setglobal(L, "rooted_uv_force_double_literal");
  lua_pushboolean(L, 1);
  lua_setglobal(L, "rooted_upvalue_admission");
  lua_pushstring(L, argv[2]);
  lua_setglobal(L, "rooted_upvalue_mode");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "rooted upvalue admission fixture: %s\n",
	    lua_tostring(L, -1));
    abort();
  }
  arm64_fixture_init(&f, L, "rooted_upvalue_trace_clone");
  f.view.proto = trace_startpt_acq(f.owner);
  f.view.startpc = trace_startpc_acq(f.owner);
  viewproto = f.view.proto;
  viewstartpc = f.view.startpc;
  /* The helper fixture normally shares snapshots between the two passes.
  ** Separate them here so each snapshot corruption can be tested against one
  ** validator while the other still sees the pristine view. */
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
  assert(tail > REF_FIRST && f.ir[tail].o == IR_CARG);
  head = f.ir[tail].op1;
  assert(head > REF_FIRST && f.ir[head].o == IR_CARG);
  root1 = f.ir[head].op1;
  index = f.ir[head].op2;
  root2 = get ? 0 : f.ir[tail].op2;
  assert(root1 > REF_FIRST && root1 < head && index >= f.semantic.nk &&
	 index < REF_TRUE && f.ir[index].o == IR_KINT &&
	 f.ir[index].t.irt == IRT_INT);
  assert(f.ir[root1].o == IR_TMPREF && f.ir[root1].t.irt == IRT_PGC &&
	 f.ir[root1].op2 == (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) :
					      IRTMPREF_IN1));
  source = f.ir[root1].op1;
  assert(source > REF_FIRST && source < root1 &&
	 f.ir[source].o == IR_SLOAD && f.ir[source].op1 == 0 &&
	 f.ir[source].t.irt == IRT_FUNC &&
	 f.ir[source].op2 == IRSLOAD_READONLY);
  assert(f.ir[tail].op2 == (get ? root1 : root2));
  assert(!get || (root2 == 0));
  if (!get) {
    assert(root2 > REF_FIRST && root2 < tail &&
	   f.ir[root2].o == IR_TMPREF &&
	   f.ir[root2].op2 == IRTMPREF_IN2);
  }

  guard = call + 1;
  assert(f.ir[guard].o == IR_NE &&
	 f.ir[guard].t.irt == (IRT_INT|IRT_GUARD));
  statusk = f.ir[guard].op1 == call ? f.ir[guard].op2 : f.ir[guard].op1;
  assert(statusk >= f.semantic.nk && statusk < REF_TRUE &&
	 f.ir[statusk].o == IR_KINT && f.ir[statusk].i == 0);
  if (get) {
    result = call + 2;
    assert(f.ir[result].o == IR_VLOAD && f.ir[result].op1 == root1 &&
	   f.ir[result].op2 == 0 && irt_isguard(f.ir[result].t));
  }
  pre = find_latest_snapshot(&f, call + 1);
  assert(f.snaps[pre].ref == root1);
  post = find_snapshot(&f, call + 2);
  assert(pre < post);
  assert(f.snaps[post].ref == call + 2);
  prepc = snap_pc(&f.map[f.snaps[pre].mapofs + f.snaps[pre].nent]);
  postpc = snap_pc(&f.map[f.snaps[post].mapofs + f.snaps[post].nent]);
  assert((get && bc_op(*prepc) == BC_UGET && postpc == prepc) ||
	 (!get && (bc_op(*prepc) == BC_USETV ||
		   bc_op(*prepc) == BC_USETS ||
		   bc_op(*prepc) == BC_USETN ||
		   bc_op(*prepc) == BC_USETP) &&
	  postpc == prepc + 1 && f.snaps[post].count == SNAPCOUNT_DONE));
  if (!get) {
    BCOp expected = !strcmp(argv[2], "literal-string") ? BC_USETS :
	    (literal_knum || literal_kint) ? BC_USETN :
	    literal ? BC_USETP : BC_USETV;
    assert(bc_op(*prepc) == expected);
  }
  assert((!forl && bc_op(f.view.startins) == BC_LOOP) ||
	 (forl && bc_op(f.view.startins) == BC_FORL));
  uv = get ? bc_d(*prepc) : bc_a(*prepc);
  assert(uv < f.J->pt->sizeuv && f.ir[index].i == (int32_t)uv);
  assert(uv == 0 && f.J->pt->sizeuv == 1);
  if (!get) {
    for (IRRef r = f.semantic.nk; r < REF_TRUE; r++)
      if (f.ir[r].o == IR_KINT && f.ir[r].t.irt == IRT_INT &&
	  f.ir[r].i != (int32_t)uv) {
	wrong_index = r;
	break;
      }
    assert(wrong_index && f.ir[wrong_index].i != f.ir[index].i);
  }
  if (!get && literal_knum) {
    IRRef stored = f.ir[root2].op1;
    assert(stored >= f.semantic.nk && stored < REF_TRUE &&
           f.ir[stored].o == IR_KNUM && f.ir[stored].t.irt == IRT_NUM);
  }
  if (!get && literal_kint) {
    IRRef stored = f.ir[root2].op1;
    assert(stored >= f.semantic.nk && stored < REF_TRUE &&
           f.ir[stored].o == IR_KINT && f.ir[stored].t.irt == IRT_INT);
  }
  if (!get && !literal) {
    IRRef stored = f.ir[root2].op1;
    assert(stored >= REF_FIRST && stored < root2 &&
	   (f.ir[stored].o == IR_SLOAD || f.ir[stored].o == IR_CONV));
  }

  mapbytes = f.semantic.nsnapmap * sizeof(SnapEntry);
  savedmap = (SnapEntry *)malloc(mapbytes);
  savedpostmap = (SnapEntry *)malloc(mapbytes);
  assert(savedmap && savedpostmap);
  memcpy(savedmap, f.map, mapbytes);
  memcpy(savedpostmap, f.postmap, mapbytes);

#define RESET() do { \
  arm64_fixture_reset(&f); \
  f.view.proto = viewproto; f.view.startpc = viewstartpc; \
  memcpy(viewsnaps, savedviewsnaps, f.snapbytes); \
  memcpy(f.map, savedmap, mapbytes); \
  memcpy(f.postmap, savedpostmap, mapbytes); \
  assert(semantic(&f) && allocated(&f)); \
} while (0)
#define SEM_REJECT(change) do { RESET(); change; \
  sem = semantic(&f); postra = allocated(&f); \
  if (sem || !postra) { fprintf(stderr, "semantic mutation accepted line %d\n", __LINE__); abort(); } \
} while (0)
#define POST_REJECT(change) do { RESET(); change; \
  sem = semantic(&f); postra = allocated(&f); \
  if (!sem || postra) { fprintf(stderr, "postRA mutation accepted line %d\n", __LINE__); abort(); } \
} while (0)
  /* The semantic pass reads this private dumped-prototype clone, while the
  ** allocator pass remains bound to the untouched published owner proto. */
  {
    BCIns *sourcebc = (BCIns *)(void *)prepc;
    BCIns original = *sourcebc;
    BCIns otherop = (original & ~(BCIns)0xffu) |
                    (BCIns)(get ? BC_USETP : BC_UGET);
    BCIns otheruv;
    uint32_t oldsizeuv = f.J->pt->sizeuv;
    SEM_REJECT(*sourcebc = otherop);
    *sourcebc = original;
    if (get)
      otheruv = (original & 0x0000ffffu) | ((BCIns)(uv+1u) << 16);
    else
      otheruv = (original & ~0x0000ff00u) | ((BCIns)(uv+1u) << 8);
    f.J->pt->sizeuv = uv+2u;  /* Keep the changed source index in range. */
    SEM_REJECT(*sourcebc = otheruv);
    *sourcebc = original;
    f.J->pt->sizeuv = oldsizeuv;
  }
  if (!get && (!strcmp(argv[2], "literal-negzero") ||
               !strcmp(argv[2], "literal-double-integer"))) {
    IRRef alias = f.semantic.nk-1u;
    assert(alias < REF_TRUE && alias < f.nins);
    IRRef old_semantic_nk = f.semantic.nk;
    IRRef old_view_nk = f.view.nk;
    IRIns old_semantic_constant = f.ir[alias];
    IRIns old_postra_constant = f.postra[alias];
    int32_t same_integer =
      !strcmp(argv[2], "literal-double-integer") ? 73 : 0;
    /* Add a private, properly tagged KINT constant with the same mathematical
    ** value. This isolates the USETN KNUM provenance requirement without
    ** changing a shared loop-step/status constant. */
    SEM_REJECT(set_test_kint(&f.ir[alias], same_integer);
               f.semantic.nk = alias; f.ir[root2].op1 = alias);
    f.semantic.nk = old_semantic_nk;
    f.ir[alias] = old_semantic_constant;
    POST_REJECT(set_test_kint(&f.postra[alias], same_integer);
                f.view.nk = alias; f.postra[root2].op1 = alias);
    f.view.nk = old_view_nk;
    f.postra[alias] = old_postra_constant;
  }
  if (get) {
    SEM_REJECT(f.ir[call].op2 = IRCALL_lj_func_storeuv_rooted_try);
    POST_REJECT(f.postra[call].op2 = IRCALL_lj_func_storeuv_rooted_try);
  } else {
    SEM_REJECT(f.ir[call].op2 = IRCALL_lj_func_getuv_rooted_try);
    POST_REJECT(f.postra[call].op2 = IRCALL_lj_func_getuv_rooted_try);
  }
  SEM_REJECT(f.ir[root1].op2 = get ? IRTMPREF_IN1 :
					      (IRTMPREF_IN1|IRTMPREF_OUT1));
  POST_REJECT(f.postra[root1].op2 = get ? IRTMPREF_IN1 :
						 (IRTMPREF_IN1|IRTMPREF_OUT1));
  SEM_REJECT(f.ir[root1].op1 = index);
  POST_REJECT(f.postra[root1].op1 = index);
  SEM_REJECT(f.ir[source].op1 = 1);
  POST_REJECT(f.postra[source].op1 = 1);
  SEM_REJECT(f.ir[source].op2 = IRSLOAD_TYPECHECK);
  POST_REJECT(f.postra[source].op2 = IRSLOAD_TYPECHECK);
  SEM_REJECT(f.ir[source].t.irt = IRT_TAB);
  POST_REJECT(f.postra[source].t.irt = IRT_TAB);
  SEM_REJECT(f.ir[index].i = (int32_t)uv + 1);
  POST_REJECT(f.postra[index].i = (int32_t)uv + 1);
  SEM_REJECT(f.ir[head].op2 = wrong_index);
  POST_REJECT(f.postra[head].op2 = wrong_index);
  SEM_REJECT(f.ir[index].t.irt = IRT_NUM);
  POST_REJECT(f.postra[index].t.irt = IRT_NUM);
  SEM_REJECT(f.ir[tail].op1 = root1);
  POST_REJECT(f.postra[tail].op1 = root1);
  SEM_REJECT(f.ir[head].op1 = index);
  POST_REJECT(f.postra[head].op1 = index);
  SEM_REJECT(f.ir[head].op2 = root1);
  POST_REJECT(f.postra[head].op2 = root1);
  SEM_REJECT(f.ir[call].op1 = index);
  POST_REJECT(f.postra[call].op1 = index);
  SEM_REJECT(f.ir[guard].o = IR_EQ);
  POST_REJECT(f.postra[guard].o = IR_EQ);
  SEM_REJECT(if (f.ir[guard].op1 == call) f.ir[guard].op1 = root1;
	     else f.ir[guard].op2 = root1);
  POST_REJECT(if (f.postra[guard].op1 == call) f.postra[guard].op1 = root1;
	      else f.postra[guard].op2 = root1);

  if (get) {
    SEM_REJECT(f.ir[result].op1 = index);
    POST_REJECT(f.postra[result].op1 = index);
    SEM_REJECT(f.ir[result].op2 = 1);
    POST_REJECT(f.postra[result].op2 = 1);
    SEM_REJECT(f.ir[result].t.irt = IRT_PROTO|IRT_GUARD);
    POST_REJECT(f.postra[result].t.irt = IRT_PROTO|IRT_GUARD);
    SEM_REJECT(f.ir[result].t.irt &= (IRType)~IRT_GUARD);
    POST_REJECT(f.postra[result].t.irt &= (IRType)~IRT_GUARD);
  } else {
    SEM_REJECT(f.ir[root2].op2 = IRTMPREF_IN1);
    POST_REJECT(f.postra[root2].op2 = IRTMPREF_IN1);
    {
      IRRef wrong_value = f.ir[root2].op1 == REF_TRUE ? REF_FALSE : REF_TRUE;
      SEM_REJECT(f.ir[root2].op1 = wrong_value);
      POST_REJECT(f.postra[root2].op1 = wrong_value);
    }
    SEM_REJECT(f.ir[tail].op2 = root1);
    POST_REJECT(f.postra[tail].op2 = root1);
    SEM_REJECT(f.ir[call].t.irt = IRT_INT);
    POST_REJECT(f.postra[call].t.irt = IRT_INT);
  }

  SEM_REJECT(f.snaps[pre].ref = call);
  POST_REJECT(viewsnaps[pre].ref = call);
  SEM_REJECT(f.snaps[post].ref = call + 1);
  POST_REJECT(viewsnaps[post].ref = call + 1);
  SEM_REJECT(offset_snapshot_pc(f.map, &f.snaps[pre], 1));
  POST_REJECT(offset_snapshot_pc(f.postmap, &viewsnaps[pre], 1));
  SEM_REJECT(offset_snapshot_pc(f.map, &f.snaps[post], 1));
  POST_REJECT(offset_snapshot_pc(f.postmap, &viewsnaps[post], 1));
  if (!get) {
    SEM_REJECT(f.snaps[post].count = 0);
    POST_REJECT(viewsnaps[post].count = 0);
  }

  /* Post-RA independently certifies scratch locations: output and roots must
  ** stay in GPRs, the two store roots cannot alias, and no root may spill. */
  POST_REJECT(f.postra[root1].r = RID_D0);
  POST_REJECT(f.postra[root1].s = SPS_LIMIT-1);
  POST_REJECT(f.postra[source].r = RID_D0);
  POST_REJECT(f.postra[source].s = SPS_LIMIT-1);
  if (get) {
    IRType result_type = irt_type(f.ir[result].t);
    POST_REJECT(f.postra[result].r =
	result_type == IRT_NUM ? RID_X0 : RID_D0);
    POST_REJECT(f.postra[result].s = SPS_LIMIT-1);
  } else {
    assert(f.postra[root1].r != f.postra[root2].r);
    POST_REJECT(f.postra[root2].r = f.postra[root1].r);
    POST_REJECT(f.postra[root2].r = RID_D0);
    POST_REJECT(f.postra[root2].s = SPS_LIMIT-1);
  }
  POST_REJECT(f.postra[call].r = RID_D0);
  POST_REJECT(f.view.proto = NULL);
  POST_REJECT(f.view.startpc = f.view.startpc + 1);

  RESET();
  f.J->pt->sizeuv = 0;
  sem = semantic(&f);
  postra = allocated(&f);
  f.J->pt->sizeuv = 1;
  if (sem || !postra) {
    fprintf(stderr, "semantic upvalue bound accepted line %d\n", __LINE__);
    abort();
  }

  RESET();
  assert(semantic(&f) && allocated(&f));
  printf("rooted upvalue %s admission: call=%u root=%u index=%u\n",
	 argv[2], (unsigned)call, (unsigned)root1, (unsigned)index);
  arm64_fixture_free(&f);
  free(savedviewsnaps);
  free(viewsnaps);
  free(savedmap);
  free(savedpostmap);
  lua_close(L);
  puts("t-arm64-jit-rooted-upvalue-admission OK");
  return 0;
}
