/* Independent semantic and allocated-layout checks for no-meta nil reads. */
#include "lauxlib.h"
#include "lib/arm64_trace_fixture.h"
#include "lj_ircall.h"
#include "lj_target.h"
#include "lua.h"
#include "lualib.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "rooted nil-read admission requires experimental ARM64 profile"
#endif

static int semantic(Arm64TraceFixture *f) {
  LJArm64IRReject reject;
  return lj_asm_arm64_ir_admit(f->J, &f->semantic, &reject);
}

static int allocated(Arm64TraceFixture *f) {
  IRRef nins = 0;
  return lj_asm_arm64_postra_admit(&f->view, &nins);
}

static IRRef find_call(Arm64TraceFixture *f) {
  IRRef r;
  for (r = REF_FIRST; r < f->semantic_nins; r++)
    if (f->ir[r].o == IR_CALLS &&
        f->ir[r].op2 == IRCALL_lj_tab_getnil_nometa_rooted_try)
      return r;
  assert(!"missing nil/no-metatable rooted helper call");
  return 0;
}

static SnapNo find_result_snapshot(Arm64TraceFixture *f, IRRef ref) {
  SnapNo s;
  for (s = 0; s < f->semantic.nsnap; s++)
    if (f->snaps[s].ref == ref)
      return s;
  assert(!"missing post-helper nil result snapshot");
  return (SnapNo)~0u;
}

static unsigned find_nil_snapshot_entry(Arm64TraceFixture *f, SnapNo s,
                                        int forl) {
  /* The while functions declare i, count, value; the for function has
  ** count, value before its loop-control slots. */
  BCReg value_slot = 1 + LJ_FR2 + f->J->pt->numparams + (forl ? 1 : 2);
  /* The immediate type-guard snapshot precedes the recorder's destination
  ** assignment. Require the returned value slot in a later snapshot. */
  for (s++; s < f->semantic.nsnap; s++) {
    SnapShot *snap = &f->snaps[s];
    unsigned i;
    for (i = 0; i < snap->nent; i++)
      if (snap_slot(f->map[snap->mapofs + i]) == value_slot &&
          snap_ref(f->map[snap->mapofs + i]) == REF_NIL)
        return snap->mapofs + i;
  }
  assert(!"nil result snapshot lacks explicit REF_NIL value slot");
  return 0;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef read, result, tail, head, table, key, keyvalue, status_guard, nil_value;
  IRRef nilarg = 0, r;
  SnapNo result_snap;
  unsigned nil_map_idx;
  SnapEntry original_nil_entry;
  int sload_key, matrix, nil_arg, forl;
  const char *clone_global;
  assert(L && argc == 3);
  forl = !strcmp(argv[2], "forl");
  sload_key = !strcmp(argv[2], "sload") || forl;
  nil_arg = !strcmp(argv[2], "nilarg");
  matrix = !strcmp(argv[2], "matrix");
  assert(matrix || sload_key || nil_arg || !strcmp(argv[2], "nilkey"));
  clone_global = forl                   ? "rooted_nil_forl_trace_clone"
                 : sload_key || nil_arg ? "rooted_nil_read_trace_clone"
                                        : "rooted_nil_key_trace_clone";
  lua_pushlightuserdata(L, (void *)(uintptr_t)0x0000010000005678ULL);
  lua_setglobal(L, "lightud_segmented");
  luaL_openlibs(L);
  lua_pushboolean(L, !matrix);
  lua_setglobal(L, "rooted_nil_read_admission");
  lua_pushboolean(L, !sload_key && !nil_arg && !matrix);
  lua_setglobal(L, "rooted_nil_key_admission");
  lua_pushboolean(L, nil_arg);
  lua_setglobal(L, "rooted_nil_arg_admission");
  lua_pushboolean(L, forl);
  lua_setglobal(L, "rooted_nil_forl_admission");
  lua_pushinteger(L, IRT_NIL);
  lua_setglobal(L, "rooted_nil_irtype");
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "rooted nil-read admission fixture: %s\n",
            lua_tostring(L, -1));
    abort();
  }
  if (matrix) {
    lua_close(L);
    puts("t-arm64-jit-rooted-nil-read matrix driver OK");
    return 0;
  }

  arm64_fixture_init(&f, L, clone_global);
  assert(bc_op(f.view.startins) == (forl ? BC_FORL : BC_LOOP));
  assert(semantic(&f) && allocated(&f));
  read = find_call(&f);
  tail = f.ir[read].op1;
  assert(f.ir[tail].o == IR_CARG);
  head = f.ir[tail].op1;
  assert(f.ir[head].o == IR_CARG);
  table = f.ir[head].op1;
  key = f.ir[head].op2;
  assert(f.ir[table].o == IR_TMPREF &&
         f.ir[table].op2 == (IRTMPREF_IN1 | IRTMPREF_OUT1));
  assert(f.ir[key].o == IR_TMPREF && f.ir[key].op2 == IRTMPREF_IN2);
  assert(f.ir[tail].op2 == table);
  keyvalue = f.ir[key].op1;
  if (sload_key) {
    assert(irt_type(f.ir[keyvalue].t) == IRT_TAB &&
           (f.ir[keyvalue].t.irt & IRT_GUARD));
  } else {
    assert(keyvalue == REF_NIL);
  }
  if (nil_arg) {
    for (r = REF_FIRST; r < f.semantic_nins; r++)
      if (f.ir[r].o == IR_SLOAD && f.ir[r].t.irt == (IRT_NIL | IRT_GUARD) &&
          f.ir[r].op1 == 2 + LJ_FR2) {
        nilarg = r;
        break;
      }
    assert(nilarg);
  }

  status_guard = read + 1;
  result = read + 2;
  assert(f.ir[status_guard].o == IR_NE);
  assert(f.ir[result].o == IR_VLOAD && irt_type(f.ir[result].t) == IRT_NIL);
  nil_value = (f.ir[status_guard].op1 == read) ? f.ir[status_guard].op2
                                               : f.ir[status_guard].op1;
  assert(irref_isk(nil_value) && f.ir[nil_value].o == IR_KINT &&
         f.ir[nil_value].i == 0);
  result_snap = find_result_snapshot(&f, result);
  nil_map_idx = find_nil_snapshot_entry(&f, result_snap, forl);
  original_nil_entry = f.map[nil_map_idx];

#define RESET()                                                                \
  do {                                                                         \
    arm64_fixture_reset(&f);                                                   \
    assert(semantic(&f) && allocated(&f));                                     \
  } while (0)
#define SEM_REJECT(change)                                                     \
  do {                                                                         \
    RESET();                                                                   \
    change;                                                                    \
    assert(!semantic(&f));                                                     \
    assert(allocated(&f));                                                     \
  } while (0)
#define POST_REJECT(change)                                                    \
  do {                                                                         \
    RESET();                                                                   \
    change;                                                                    \
    assert(semantic(&f));                                                      \
    assert(!allocated(&f));                                                    \
  } while (0)

  SEM_REJECT(f.ir[read].op2 = IRCALL_lj_tab_gettv_rooted_hit_try);
  POST_REJECT(f.postra[read].op2 = IRCALL_lj_tab_gettv_rooted_hit_try);
  SEM_REJECT(f.ir[table].op2 = IRTMPREF_IN2);
  POST_REJECT(f.postra[table].op2 = IRTMPREF_IN2);
  SEM_REJECT(f.ir[key].op2 = IRTMPREF_IN1);
  POST_REJECT(f.postra[key].op2 = IRTMPREF_IN1);
  SEM_REJECT(f.ir[head].op1 = key);
  POST_REJECT(f.postra[head].op1 = key);
  SEM_REJECT(f.ir[head].op2 = table);
  POST_REJECT(f.postra[head].op2 = table);
  SEM_REJECT(f.ir[tail].op2 = key);
  POST_REJECT(f.postra[tail].op2 = key);
  SEM_REJECT(f.ir[status_guard].o = IR_EQ);
  POST_REJECT(f.postra[status_guard].o = IR_EQ);
  SEM_REJECT(f.ir[nil_value].i = 1);
  POST_REJECT(f.postra[nil_value].i = 1);
  SEM_REJECT(f.ir[result].t.irt = IRT_TAB | IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_TAB | IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt = IRT_NUM | IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_NUM | IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt &= (IRType)~IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt &= (IRType)~IRT_GUARD);
  /* A primitive nil carrier cannot be assigned to an FPR in allocated IR. */
  POST_REJECT(f.postra[result].r = RID_D0);
  POST_REJECT(f.postra[result].r = RID_X0);
  /* A canonical primitive cannot own even an otherwise valid spill slot. */
  POST_REJECT(f.postra[result].s = SPS_FIRST);
  /* The selected guarded key is a rooted SLOAD. Dropping its type guard must
  ** fail both independently reconstructed validators. */
  if (sload_key) {
    SEM_REJECT(f.ir[keyvalue].t.irt &= (IRType)~IRT_GUARD);
    POST_REJECT(f.postra[keyvalue].t.irt &= (IRType)~IRT_GUARD);
  } else {
    SEM_REJECT(f.ir[key].op1 = REF_BASE);
    POST_REJECT(f.postra[key].op1 = REF_BASE);
    SEM_REJECT(f.ir[REF_NIL].t.irt = IRT_FALSE);
    POST_REJECT(f.postra[REF_NIL].t.irt = IRT_FALSE);
  }
  if (nilarg) {
    SEM_REJECT(f.ir[nilarg].t.irt &= (IRType)~IRT_GUARD);
    POST_REJECT(f.postra[nilarg].t.irt &= (IRType)~IRT_GUARD);
    POST_REJECT(f.postra[nilarg].r = RID_X0);
    POST_REJECT(f.postra[nilarg].s = SPS_FIRST);
  }

  /* Check the exact post-helper snapshot and explicit REF_NIL map entry in
  ** each independent pass; reset shared source data between the checks. */
  RESET();
  f.snaps[result_snap].ref = REF_DROP;
  assert(!semantic(&f));
  RESET();
  f.snaps[result_snap].ref = REF_DROP;
  assert(!allocated(&f));
  RESET();
  f.map[nil_map_idx] = snap_setref(original_nil_entry, REF_DROP);
  assert(!semantic(&f));
  f.map[nil_map_idx] = original_nil_entry;
  RESET();
  f.postmap[nil_map_idx] = snap_setref(original_nil_entry, REF_DROP);
  assert(!allocated(&f));
  f.postmap[nil_map_idx] = original_nil_entry;
  RESET();
  assert(semantic(&f) && allocated(&f));
#undef POST_REJECT
#undef SEM_REJECT
#undef RESET

  printf("rooted nil-read admission: call=%u result=%u snapshot=%u\n",
         (unsigned)read, (unsigned)result, (unsigned)result_snap);
  arm64_fixture_free(&f);
  lua_close(L);
  puts("t-arm64-jit-rooted-nil-read-admission OK");
  return 0;
}
