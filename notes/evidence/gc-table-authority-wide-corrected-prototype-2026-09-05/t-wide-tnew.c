#define main original_tnew_main
#include "tnew-original.c"
#undef main
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wide-guards.h"

static void high_cell_case(uint32_t cell, int pending)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCArena *a;
  LJGC2TabStamp *s;
  LJGC2TableTokenTicket ticket = { 0 };
  uint64_t token;
  uint32_t calls;
  GCtab *result;
  assert(L);
  luaL_openlibs(L);
  ljt_lua_dostring(L, "jit.off(true, true)");
  assert(lua_checkstack(L, 64));
  g = G(L); tg = L2TG(L);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  load_empty_table_chunk(L);
  prime_traversable_bump_window(tg);
  tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].cell = cell;
  a = tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a;
  la_store64_rel(&tg->local_total, 0);
  token = poison_recycled_empty_table_fields(a, cell);
  s = lj_arena_gc2_stamp_acq(lj_arena_cellptr(a, cell));
  la_store64_rel(&s->era, UINT64_C(0xfedcba9876543210));
  wide_guards_arm(a, cell, 1u);
  if (pending) {
    assert(lj_gc2_table_token_refresh(&s->token, &ticket) == LJ_GC2_TABLE_TOKEN_RESULT_OK);
    token = ticket.control;
  }
  calls = lj_tab_test_new0_calls();
  ljt_lua_pcall(L, 0, 1, "wide high-cell TNEW");
  assert(tvistab(L->top - 1));
  result = tabV(L->top - 1);
  wide_guards_check();
  assert(la_load64_acq(&s->token.control) == token);
  if (!pending) {
    assert(lj_tab_test_new0_calls() == calls);
    assert((void *)result == lj_arena_cellptr(a, cell));
    assert(la_load64_acq(&s->state) == 0);
    assert(la_load64_acq(&s->era) == 0);
  } else {
    assert(lj_tab_test_new0_calls() == calls + 1u);
    assert((void *)result != lj_arena_cellptr(a, cell));
    assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
    assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
    assert(!lj_arena_ready_get(a, cell));
    assert(la_load64_acq(&s->state) != 0);
    assert(la_load64_acq(&s->era) == UINT64_C(0xfedcba9876543210));
    assert(lj_gc2_table_token_complete(&s->token, &ticket) == LJ_GC2_TABLE_TOKEN_RESULT_OK);
  }
  assert_empty_table_body(g, result);
  lua_close(L);
  printf("TNEW cell %u pending %d: exact full reset/token preservation/neighbor guards passed\n", cell, pending);
}

int main(int argc, char **argv)
{
  alarm(30);
  if (argc > 1 && !strcmp(argv[1], "existing")) return original_tnew_main();
  if (argc > 1) {
    high_cell_case((uint32_t)strtoul(argv[1], NULL, 10), argc > 2 ? atoi(argv[2]) : 0);
  } else {
    high_cell_case(1536u, 0);
    high_cell_case(1537u, 0);
    high_cell_case(1536u, 1);
    high_cell_case(1537u, 1);
  }
  return 0;
}
