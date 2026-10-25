/* Independent semantic/post-RA admission for exact lightuserdata
** rooted-read keys/results, in either guarded SLOAD or generic VLOAD form. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_ircall.h"
#include "lj_target.h"
#include "lib/arm64_trace_fixture.h"
#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "lightuserdata admission requires experimental ARM64 profile"
#endif

static int check_identity(lua_State *L)
{
  if (lua_type(L, 1) != LUA_TLIGHTUSERDATA ||
      lua_type(L, 2) != LUA_TLIGHTUSERDATA)
    return luaL_error(L, "expected two lightuserdata values");
  lua_pushboolean(L, lua_touserdata(L, 1) == lua_touserdata(L, 2));
  return 1;
}
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

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  Arm64TraceFixture f;
  IRRef call = 0, tail, head, table, key, keyvalue, result, lightspill = 0, r;
  SnapNo result_snap = (SnapNo)~0u, s;
  unsigned nlightspills = 0;
  int pressure;
  assert(L && argc == 3);
  pressure = !strcmp(argv[2], "admission");
  assert(pressure || !strcmp(argv[2], "admission_sload") ||
         !strcmp(argv[2], "matrix"));
  /* Seed before opening libraries so token 0 and the high-bits segment are
  ** unambiguous. These are values only; the addresses are never dereferenced.
  */
  lua_pushlightuserdata(L, (void *)(uintptr_t)0x1234);
  lua_setglobal(L, "lightud_zero");
  lua_pushlightuserdata(L, (void *)(uintptr_t)0x0000010000005678ULL);
  lua_setglobal(L, "lightud_segmented");
  {
    unsigned i;
    for (i = 1; i <= 12; i++) {
      char name[32];
      uintptr_t p = (uintptr_t)0x0000010000008000ULL + (uintptr_t)i * 16;
      snprintf(name, sizeof(name), "lightud_key%u", i);
      lua_pushlightuserdata(L, (void *)p);
      lua_setglobal(L, name);
    }
  }
  lua_pushcfunction(L, check_identity);
  lua_setglobal(L, "check_lightud_identity");
  lua_pushinteger(L, IRT_LIGHTUD);
  lua_setglobal(L, "rooted_lightud_irtype");
  lua_getglobal(L, "lightud_zero");
  lua_getglobal(L, "lightud_segmented");
  assert(tvislightud(L->top - 2) && tvislightud(L->top - 1));
  assert(lightudseg((L->top - 2)->u64) == 0 &&
         lightudseg((L->top - 1)->u64) != 0);
  assert(lightudV(G(L), L->top - 2) == (void *)(uintptr_t)0x1234);
  assert(lightudV(G(L), L->top - 1) ==
         (void *)(uintptr_t)0x0000010000005678ULL);
  lua_pop(L, 2);
  luaL_openlibs(L);
  if (pressure) {
    lua_pushboolean(L, 1);
    lua_setglobal(L, "rooted_lightud_read_admission");
  } else if (!strcmp(argv[2], "admission_sload")) {
    lua_pushboolean(L, 1);
    lua_setglobal(L, "rooted_lightud_sload_admission");
  }
  if (luaL_dofile(L, argv[1])) {
    fprintf(stderr, "lightuserdata Lua fixture: %s\n", lua_tostring(L, -1));
    return 2;
  }
  if (!strcmp(argv[2], "matrix")) {
    puts("t-arm64-jit-rooted-lightud-read matrix driver OK");
    lua_close(L);
    return 0;
  }
  arm64_fixture_init(&f, L, "rooted_lightud_read_trace_clone");
  assert(semantic(&f) && allocated(&f));
  /* Pick the generic read whose exact IN2 carrier is the intended source:
  ** SLOAD for one driver mode, or the prior rooted LIGHTUD VLOAD in pressure.
  */
  for (r = REF_FIRST; r < f.semantic_nins; r++) {
    if (f.ir[r].o == IR_CALLS &&
        f.ir[r].op2 == IRCALL_lj_tab_gettv_rooted_hit_try) {
      IRRef ct = f.ir[r].op1, ch, k, kv;
      if (f.ir[ct].o != IR_CARG)
        continue;
      ch = f.ir[ct].op1;
      if (f.ir[ch].o != IR_CARG)
        continue;
      k = f.ir[ch].op2;
      if (f.ir[k].o != IR_TMPREF)
        continue;
      kv = f.ir[k].op1;
      if (irt_type(f.ir[kv].t) == IRT_LIGHTUD &&
          ((pressure && f.ir[kv].o == IR_VLOAD) ||
           (!pressure && f.ir[kv].o == IR_SLOAD))) {
        call = r;
        tail = ct;
        head = ch;
        key = k;
        keyvalue = kv;
        break;
      }
    }
  }
  assert(call);
  table = f.ir[head].op1;
  result = call + 2;
  assert(f.ir[result].o == IR_VLOAD && irt_type(f.ir[result].t) == IRT_NUM);
  assert((f.ir[keyvalue].t.irt & IRT_GUARD) != 0);
  for (r = f.owner->nk; r < f.nins; r++) {
    IRIns p = f.postra[r];
    if ((p.o == IR_SLOAD || p.o == IR_VLOAD) && irt_type(p.t) == IRT_LIGHTUD &&
        ra_hasspill(p.s)) {
      nlightspills++;
      if (!lightspill)
        lightspill = r;
    }
  }
  assert(!pressure || (nlightspills && lightspill));
  for (s = 0; s < f.semantic.nsnap; s++)
    if (f.snaps[s].ref == result) {
      result_snap = s;
      break;
    }
  assert(result_snap != (SnapNo)~0u);

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
  SEM_REJECT(f.ir[call].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  POST_REJECT(f.postra[call].op2 = IRCALL_lj_tab_getscalar_rooted_try);
  SEM_REJECT(f.ir[table].op2 = IRTMPREF_IN2);
  POST_REJECT(f.postra[table].op2 = IRTMPREF_IN2);
  SEM_REJECT(f.ir[key].op2 = IRTMPREF_IN1);
  POST_REJECT(f.postra[key].op2 = IRTMPREF_IN1);
  SEM_REJECT(f.ir[call + 1].o = IR_EQ);
  POST_REJECT(f.postra[call + 1].o = IR_EQ);
  /* Prove lightuserdata isn't widened into arbitrary producer classes. */
  SEM_REJECT(f.ir[keyvalue].o = IR_ADD);
  POST_REJECT(f.postra[keyvalue].o = IR_ADD);
  SEM_REJECT(f.ir[keyvalue].o = IR_CONV);
  POST_REJECT(f.postra[keyvalue].o = IR_CONV);
  SEM_REJECT(f.ir[keyvalue].t.irt = IRT_PROTO | IRT_GUARD);
  POST_REJECT(f.postra[keyvalue].t.irt = IRT_PROTO | IRT_GUARD);
  SEM_REJECT(f.ir[keyvalue].t.irt = IRT_PTR | IRT_GUARD);
  POST_REJECT(f.postra[keyvalue].t.irt = IRT_PTR | IRT_GUARD);
  SEM_REJECT(f.ir[keyvalue].t.irt &= (IRType)~IRT_GUARD);
  POST_REJECT(f.postra[keyvalue].t.irt &= (IRType)~IRT_GUARD);
  SEM_REJECT(f.ir[result].t.irt = IRT_PROTO | IRT_GUARD);
  POST_REJECT(f.postra[result].t.irt = IRT_PROTO | IRT_GUARD);
  /* Exact result snapshot and typed root argument are part of the contract. */
  RESET();
  f.snaps[result_snap].ref = REF_DROP;
  assert(!semantic(&f));
  RESET();
  f.snaps[result_snap].ref = REF_DROP;
  assert(!allocated(&f));
  SEM_REJECT(f.ir[tail].op2 = head);
  POST_REJECT(f.postra[tail].op2 = head);
  /* The scalar status is 32-bit; placing it on the upper half of a live 64-bit
  ** token spill must fail the allocated-layout validator independently. */
  if (lightspill) {
    POST_REJECT(f.postra[call].s = f.postra[lightspill].s + 1);
    POST_REJECT(f.postra[lightspill].s++);
  }
  RESET();
  assert(semantic(&f) && allocated(&f));
#undef POST_REJECT
#undef SEM_REJECT
#undef RESET
  printf("lightuserdata read admission (%s): spills=%u\n", argv[2],
         nlightspills);
  arm64_fixture_free(&f);
  lua_close(L);
  puts("t-arm64-jit-rooted-lightud-read-admission OK");
  return 0;
}
