#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ff.h"
#include "lj_target.h"
#include "lj_str.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lib/arm64_trace_fixture.h"

static lua_State *test_L;
static unsigned native_string_matches;
static unsigned native_string_publications;
static int negative_case;
static int negative_first_case;
static int negative_first_magnitude;
static int negative_first_last_magnitude = 1;
static int negative_first_output_width;
static int dynamic_end_case;
static int zero_first_case;

static void assert_string_ready(lua_State *L, GCstr *str, GCArena *arena)
{
  int expected_huge = negative_first_case &&
                      negative_first_output_width > 0 &&
                      lj_str_size((MSize)negative_first_output_width) >
                        LJ_HUGE_THRESHOLD;
  if (expected_huge) assert(lj_arena_ishuge(arena));
  if (lj_arena_ishuge(arena)) {
    LJHugeInfo info;
    assert(expected_huge && negative_first_magnitude == 32768);
    assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, str, &info));
    assert((info.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) ==
           (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));
  } else {
    assert(lj_arena_ready_get(arena, lj_arena_cellof(str)));
  }
}

static void string_publish(lua_State *L, GCstr *str)
{
  GCArena *arena;
  if (L != test_L || !lj_tg_load_jit_base(L2TG(L))) return;
  assert(lj_tg_jit_active_acq(L2TG(L)));
  assert(str->gct == (uint8_t)~LJ_TSTR);
  assert((negative_case || dynamic_end_case) ? str->len > 0u : str->len == 3u);
  if (negative_first_case)
    assert(negative_first_output_width > 0 &&
           str->len == (uint32_t)negative_first_output_width);
  arena = lj_arena_of(str);
  assert_string_ready(L, str, arena);
  native_string_publications++;
}

static void string_match(lua_State *L, GCRef *link, GCobj *object,
                         uintptr_t observed, uint32_t stage)
{
  GCArena *arena;
  GCstr *str = gco2str(object);
  UNUSED(link); UNUSED(observed);
  if (stage != LJ_STR_TEST_MATCH_AFTER_COMPARE) return;
  if (L != test_L || !lj_tg_load_jit_base(L2TG(L))) return;
  assert(lj_tg_jit_active_acq(L2TG(L)));
  assert(str->gct == (uint8_t)~LJ_TSTR);
  assert((negative_case || dynamic_end_case) ? str->len > 0u : str->len == 3u);
  arena = lj_arena_of(str);
  assert_string_ready(L, str, arena);
  native_string_matches++;
}

static int semantic(Arm64TraceFixture *f)
{
  LJArm64IRReject why;
  int direction;
  int ok = bc_op(f->semantic.startins) == BC_FORL ?
    lj_asm_arm64_forl_ir_admit(f->J, &f->semantic, &why, &direction) :
    lj_asm_arm64_ir_admit(f->J, &f->semantic, &why);
  return ok;
}
static int allocated(Arm64TraceFixture *f)
{
  IRRef end;
  int direction;
  return bc_op(f->view.startins) == BC_FORL ?
    lj_asm_arm64_forl_postra_admit(&f->view, &end, &direction) :
    lj_asm_arm64_postra_admit(&f->view, &end);
}
int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef r, pointer = 0, allocation = 0, bound = 0, len = 0;
  IRRef endcalc = 0, subcalc = 0, lowerguard = 0;
  IRRef firstguard = 0, lastguard = 0;
  TraceNo no;
  GCobj *byte;
  int negative, default_end;
  (void)arm64_fixture_init;
  assert(L && argc == 3);
  negative = strstr(argv[2], "negative-") != NULL;
  negative_first_case = strncmp(argv[2], "negative-first-", 15u) == 0;
  if (negative_first_case) {
    const char *value = argv[2]+15u;
    char *end;
    long first_value;
    if (strncmp(value, "default-", 8u) == 0) value += 8u;
    else if (strncmp(value, "last-", 5u) == 0) value += 5u;
    first_value = strtol(value, &end, 10);
    if (end == value) abort();
    negative_first_magnitude = (int)first_value;
    if (strncmp(argv[2]+15u, "last-", 5u) == 0) {
      char *last_end;
      long last_value;
      assert(*end == '-');
      last_value = strtol(end+1, &last_end, 10);
      if (last_end == end+1) abort();
      negative_first_last_magnitude = (int)last_value;
    }
    assert(negative_first_magnitude > 0);
    negative_first_output_width = negative_first_magnitude-
                                  negative_first_last_magnitude+1;
  }
  default_end = strstr(argv[2], "default-end") != NULL;
  zero_first_case = strncmp(argv[2], "zero-", 5u) == 0;
  negative_case = negative;
  dynamic_end_case = default_end;
  luaL_openlibs(L);
  test_L = L;
  lj_str_test_set_match_hook(string_match);
  lj_str_test_set_publish_hook(string_publish);
  lua_pushstring(L, argv[2]); lua_setglobal(L, "substring_fixture_kind");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "%s\n", lua_tostring(L,-1)); abort();
  }
  lj_str_test_set_match_hook(NULL);
  lj_str_test_set_publish_hook(NULL);
  if (negative_first_case && negative_first_output_width <= 0) {
    assert(native_string_publications == 0u);
    fprintf(stderr, "substring candidate-JIT check OK: %s width=%d, fresh publications=%u\n",
            argv[2], negative_first_output_width, native_string_publications);
    lua_close(L);
    return 0;
  }
  assert(native_string_matches > 0u);
  if (((strstr(argv[2], "while") && !(negative && zero_first_case)) ||
       (default_end && !zero_first_case)) &&
      !(negative_first_case && negative_first_output_width <= 1))
    assert(native_string_publications > 0u);
  lua_getglobal(L, "substring_trace_no");
  no = (TraceNo)lua_tointeger(L,-1); lua_pop(L,1);
  assert(no);
  arm64_fixture_init_trace(&f, L, "substring_trace_clone", no);
  f.view.proto = trace_startpt_acq(f.owner);
  f.view.startpc = trace_startpc_acq(f.owner);
  assert(semantic(&f) && allocated(&f));
  for (r = REF_FIRST; r < f.semantic_nins; r++) {
    IRIns p = f.ir[r];
    if (p.o == IR_STRREF) pointer = r;
    if (p.o == IR_SNEW) allocation = r;
    if ((p.o == ((negative || dynamic_end_case) ? IR_GE : IR_UGE)) &&
        p.t.irt == (IRT_INT|IRT_GUARD)) bound = r;
    if (p.o == IR_FLOAD && p.op2 == IRFL_STR_LEN) len = r;
    if (negative && p.o == IR_ADD && p.t.irt == IRT_INT &&
        (p.op1 == len || p.op2 == len)) endcalc = r;
    if (negative && p.o == IR_SUBOV && p.t.irt == (IRT_INT|IRT_GUARD))
      subcalc = r;
    if (negative_first_case && p.o == IR_GE &&
        p.t.irt == (IRT_INT|IRT_GUARD) && pointer && p.op1 == f.ir[pointer].op2)
      lowerguard = r;
    if (p.o == IR_EQ && p.t.irt == (IRT_FUNC|IRT_GUARD)) {
      if (!firstguard) firstguard = r;
      lastguard = r;
    }
  }
  if (negative_first_case) {
    assert(subcalc && pointer);
    endcalc = f.ir[subcalc].op1;
    lowerguard = 0;
    for (r = endcalc+1u; r < pointer; r++)
      if (f.ir[r].o == IR_GE && f.ir[r].t.irt == (IRT_INT|IRT_GUARD) &&
          f.ir[r].op1 == f.ir[pointer].op2) lowerguard = r;
  }
  assert(pointer && allocation && bound && len && firstguard &&
         lastguard > allocation);
#define BOTH(ref, field, value) \
  f.ir[ref].field = (value); f.postra[ref].field = (value)
#define REJECT(change) do { \
  arm64_fixture_reset(&f); assert(semantic(&f) && allocated(&f)); \
  change; \
  if (semantic(&f)) { \
    fprintf(stderr, "substring semantic mutation accepted at line %d\n", __LINE__); \
    abort(); \
  } \
  if (allocated(&f)) { \
    fprintf(stderr, "substring post-RA mutation accepted at line %d\n", __LINE__); \
    abort(); \
  } \
} while (0)
  REJECT(BOTH(pointer, op1, pointer));
  REJECT(BOTH(pointer, op2, f.ir[allocation].op2));
  REJECT(BOTH(allocation, op1, len));
  REJECT(BOTH(allocation, op2, f.ir[pointer].op2));
  REJECT(BOTH(allocation, t.irt, IRT_STR|IRT_GUARD));
  REJECT(BOTH(allocation, o, IR_XSNEW));
  REJECT(BOTH(bound, o, negative ? IR_UGE : IR_ULT));
  REJECT(BOTH(bound, op2, len));
  REJECT(BOTH(len, op2, IRFL_TAB_ASIZE));
  if (negative && !zero_first_case) {
    assert(endcalc && subcalc);
    if (negative_first_case) {
      IRRef startref = f.ir[pointer].op2;
      assert(f.ir[subcalc].op2 == startref);
      if (endcalc == len) {
        assert(negative_first_last_magnitude == 1);
        REJECT(BOTH(endcalc, op1, startref));
      } else {
        IRIns end = f.ir[endcalc];
        assert(end.o == IR_ADD && end.t.irt == IRT_INT);
        assert((end.op1 == len && end.op2 != startref) ||
               (end.op2 == len && end.op1 != startref));
        {
          IRIns start = f.ir[startref];
          IRRef start_constant, end_constant;
          assert(start.o == IR_ADD && start.t.irt == IRT_INT);
          start_constant = start.op1 == len ? start.op2 : start.op1;
          end_constant = end.op1 == len ? end.op2 : end.op1;
          assert(start_constant != end_constant);
          if (end.op1 == len)
            REJECT(BOTH(endcalc, op2, start_constant));
          else
            REJECT(BOTH(endcalc, op1, start_constant));
        }
        if (end.op1 == len)
          REJECT(BOTH(endcalc, op2, startref));
        else
          REJECT(BOTH(endcalc, op1, startref));
      }
      REJECT(BOTH(subcalc, op1, endcalc == len ? startref : len));
    } else {
      assert(f.ir[subcalc].op1 == endcalc);
      if (f.ir[endcalc].op1 == len)
        REJECT(BOTH(endcalc, op2, f.ir[pointer].op2));
      else
        REJECT(BOTH(endcalc, op1, f.ir[pointer].op2));
    }
    REJECT(BOTH(subcalc, op2, f.ir[bound].op2));
  }
  if (negative_first_case) {
    IRRef other_string = 0;
    assert(lowerguard && lowerguard > endcalc && lowerguard < pointer);
    REJECT(BOTH(lowerguard, o, IR_UGE));
    REJECT(BOTH(lowerguard, op2, f.ir[endcalc].op2));
    for (r = REF_FIRST; r < f.semantic_nins; r++) {
      IRIns candidate = f.ir[r];
      if (r != f.ir[pointer].op1 && candidate.o == IR_SLOAD &&
          candidate.t.irt == (IRT_STR|IRT_GUARD)) {
        other_string = r;
        break;
      }
    }
    assert(other_string);
    REJECT(BOTH(len, op1, other_string));
  }
  lua_getglobal(L, "string"); lua_getfield(L, -1, "byte");
  byte = obj2gco(funcV(L->top-1));
  REJECT(ir_kgc_store_rel(&f.ir[f.ir[firstguard].op2], byte);
         ir_kgc_store_rel(&f.postra[f.postra[firstguard].op2], byte));
  arm64_fixture_reset(&f);
  {
    IRRef oldnk = f.semantic.nk, fresh = oldnk-2u;
    assert(oldnk > 2u);
    f.semantic.nk = f.view.nk = fresh;
    ir_kgc_publish(&f.ir[fresh], byte, IRT_FUNC);
    ir_kgc_publish(&f.postra[fresh], byte, IRT_FUNC);
    assert(semantic(&f) && allocated(&f));
    /* Keep the allocation's initial guard intact, changing only the later
    ** repeated CALL guard. The result must not survive the wrong callee. */
    BOTH(lastguard, op2, fresh);
    assert(!semantic(&f) && !allocated(&f));
    f.semantic.nk = f.view.nk = oldnk;
  }
  arm64_fixture_reset(&f);
  {
    LJArm64PostRAView original_view = f.view;
    GCtrace private_owner = *f.owner;
    GCproto *private_proto = trace_startpt_acq(&f.semantic);
    BCIns *bc = proto_bc(private_proto);
    MSize callpc = 0, pc, setup;
    trace_startpt_rel(&private_owner, private_proto);
    setmref(private_owner.startpc, trace_startpc_acq(&f.semantic));
    f.view.owner = &private_owner;
    f.view.proto = private_proto;
    f.view.proto_bc = bc;
    f.view.startpc = trace_startpc_acq(&f.semantic);
    f.view.snapmap = f.map;
    f.view.owner_private = 1;
    assert(semantic(&f) && allocated(&f));
    for (pc = 1; pc < private_proto->sizebc; pc++)
      if (bc_op(bc[pc]) == BC_CALL) { assert(!callpc); callpc = pc; }
    assert(callpc >= (default_end ? 2u : 3u));
    setup = callpc-(bc_c(bc[callpc]) == 3u ? 2u : 3u);
    /* The captured builtin's UGET is independently bound by the rooted
    ** read proof. A correctly guarded constant does not excuse replacing
    ** the actual callee setup with a non-function source instruction. */
    {
      BCIns saved = bc[setup-1u];
      assert(semantic(&f) && allocated(&f));
      bc[setup-1u] = BCINS_AD(BC_KPRI, bc_a(saved), 0);
      assert(!semantic(&f) && !allocated(&f));
      bc[setup-1u] = saved;
      assert(semantic(&f) && allocated(&f));
    }
    if (zero_first_case) {
      MSize firstpc = callpc-(bc_c(bc[callpc]) == 3u ? 1u : 2u);
      BCIns saved = bc[firstpc], one = saved;
      assert(bc_op(saved) == BC_KSHORT && (int16_t)bc_d(saved) == 0);
      setbc_d(&one, 1);
      la_store32_rel((uint32_t *)&bc[firstpc], one);
      assert(semantic(&f) && allocated(&f)); /* 0 and 1 both clamp to 1. */
      la_store32_rel((uint32_t *)&bc[firstpc], saved);
      assert(semantic(&f) && allocated(&f));
    }
    {
      unsigned tested = 0;
      size_t mapbytes = f.view.nsnapmap*sizeof(SnapEntry);
      SnapEntry *postcopy = (SnapEntry *)malloc(mapbytes);
      assert(postcopy);
      memcpy(postcopy, f.map, mapbytes);
      f.view.snapmap = postcopy;
      assert(semantic(&f) && allocated(&f));
      for (SnapNo s = 0; s < f.view.nsnap; s++) {
        const SnapShot *snap = &f.snaps[s];
        IRRef at = snap_ref_acq(snap);
        MSize footer = snap_mapofs_acq(snap)+snap_nent_acq(snap);
        uint64_t saved, changed;
        if (at >= f.semantic_nins || f.ir[at].o != IR_EQ ||
            f.ir[at].t.irt != (IRT_FUNC|IRT_GUARD)) continue;
        memcpy(&saved, &f.map[footer], sizeof(saved));
        /* The string copy precedes the pure literal suffix for either
        ** arity. Every repeated CALL guard must preserve the suffix PC and
        ** root-frame footer tag, including guards after a hoisted SNEW. */
        for (unsigned mode = 0; mode < 2u; mode++) {
          changed = mode ? saved | 1u :
            (uint64_t)(uintptr_t)(bc+setup) << 8;
          assert(changed != saved);
          memcpy(&f.map[footer], &changed, sizeof(changed));
          assert(!semantic(&f) && allocated(&f));
          memcpy(&f.map[footer], &saved, sizeof(saved));
          memcpy(&postcopy[footer], &changed, sizeof(changed));
          assert(semantic(&f) && !allocated(&f));
          memcpy(&postcopy[footer], &saved, sizeof(saved));
          assert(semantic(&f) && allocated(&f));
        }
        tested++;
      }
      assert(tested >= 2u);
      f.view.snapmap = f.map;
      free(postcopy);
    }
    {
      MSize nsetup = callpc-setup;
      for (unsigned mode = 0; mode < nsetup+2u; mode++) {
        MSize at = mode < nsetup ? callpc-1u-mode : callpc;
        BCIns saved = bc[at], changed = saved;
        if (mode == 0u)
          setbc_d(&changed, zero_first_case ? -1 : bc_d(saved)-1u);
        else if (mode == 1u)
          setbc_d(&changed, zero_first_case && nsetup == 3u ? -1 :
                  (nsetup == 2u ? bc_d(saved)+1u : 0u));
        else if (mode == 2u && nsetup == 3u)
          setbc_d(&changed, bc_d(saved)+1u);
        else if (mode < nsetup) setbc_d(&changed, bc_d(saved)+1u);
        else if (mode == nsetup) setbc_b(&changed, 3u);
        else setbc_c(&changed, bc_c(saved) == 3u ? 4u : 3u);
        la_store32_rel((uint32_t *)&bc[at], changed);
        assert(!semantic(&f));
        assert(!allocated(&f));
        la_store32_rel((uint32_t *)&bc[at], saved);
        assert(semantic(&f) && allocated(&f));
      }
    }
    {
      BCIns saved, changed;
      assert(setup >= 1u);
      saved = bc[setup-1u];
      changed = BCINS_AD(BC_ISLT, 0, 1);
      la_store32_rel((uint32_t *)&bc[setup-1u], changed);
      assert(!semantic(&f));
      assert(!allocated(&f));
      la_store32_rel((uint32_t *)&bc[setup-1u], saved);
      assert(semantic(&f) && allocated(&f));
    }
    f.view = original_view;
  }
  arm64_fixture_reset(&f);
  {
    IRRef source = f.ir[pointer].op1;
    f.postra[source].r = RID_X0;
    f.postra[source].s = SPS_NONE;
    assert(semantic(&f) && !allocated(&f));
  }
  arm64_fixture_reset(&f);
  if (negative || default_end) {
    IRRef widthref = f.ir[allocation].op2;
    assert(widthref >= REF_FIRST && widthref < pointer);
    f.postra[widthref].r = RID_D0;
    f.postra[widthref].s = SPS_NONE;
    assert(semantic(&f) && !allocated(&f));
    arm64_fixture_reset(&f);
  }
  f.postra[allocation].r = RID_D0;
  assert(semantic(&f) && !allocated(&f));
  arm64_fixture_reset(&f);
  f.postra[allocation].s = SPS_FIRST+1;
  f.view.spadjust = 16;
  assert(semantic(&f) && !allocated(&f));
  f.view.spadjust = f.owner->spadjust;
  arm64_fixture_reset(&f);
  assert(semantic(&f) && allocated(&f));
  arm64_fixture_free(&f);
  lua_close(L);
  printf("substring admission OK: %s bounds, callee, snapshots and allocation\n", argv[2]);
  printf("substring native helper: %s matches=%u fresh_publications=%u\n",
         argv[2], native_string_matches, native_string_publications);
  return 0;
}
