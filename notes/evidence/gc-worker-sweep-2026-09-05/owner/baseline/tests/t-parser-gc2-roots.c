/*
** Focused lifetime test for LexState raw-root publication and unwind.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_buf.h"
#include "lj_cdata.h"
#include "lj_ctype.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_lex.h"
#include "lj_tab.h"
#include "lj_tg.h"

static const char *eof_reader(lua_State *L, void *ud, size_t *size)
{
  UNUSED(L); UNUSED(ud);
  *size = 0;
  return NULL;
}

static const char *throw_reader(lua_State *L, void *ud, size_t *size)
{
  UNUSED(ud); UNUSED(size);
  luaL_error(L, "LexState setup reader failure");
  return NULL;
}

static void init_lexstate(lua_State *L, LexState *ls)
{
  memset(ls, 0, sizeof(*ls));
  ls->rfunc = eof_reader;
  ls->chunkarg = "=(lex-root-test)";
  lj_buf_init(L, &ls->sb);
}

static void add_raw_backing(lua_State *L, LexState *ls)
{
  ls->sizebcstack = 64;
  ls->bcstack = lj_mem_newvec(L, ls->sizebcstack, BCInsLine);
  lj_lex_root_bcstack_rel(ls, ls->bcstack);
  ls->sizevstack = 64;
  ls->vstack = lj_mem_newvec(L, ls->sizevstack, VarInfo);
  lj_lex_root_vstack_rel(ls, ls->vstack);
  (void)lj_buf_need(&ls->sb, 4096);
}

static void assert_raw_backing_marked(global_State *g, LexState *ls)
{
  assert(lj_gc2_ismarkedmem(g, ls->bcstack) == 1);
  assert(lj_gc2_ismarkedmem(g, ls->vstack) == 1);
  assert(lj_gc2_ismarkedmem(g, lj_buf_bptr_acq(&ls->sb)) == 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  LexState outer, inner;
  int status;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(tg != NULL && lj_tg_lexstate_acq(tg) == NULL);

  init_lexstate(L, &outer);
  assert(lj_lex_setup(L, &outer) == 0);
  assert(lj_tg_lexstate_acq(tg) == &outer);
  add_raw_backing(L, &outer);

  init_lexstate(L, &inner);
  assert(lj_lex_setup(L, &inner) == 0);
  assert(lj_tg_lexstate_acq(tg) == &inner);
  assert(lj_lex_root_prev_acq(&inner) == &outer);
  add_raw_backing(L, &inner);

#if LJ_HASFFI
  {
    GCcdata *tokcd = lj_cdata_new_(L, CTID_INT64, 8);
    GCcdata *lookcd = lj_cdata_new_(L, CTID_UINT64, 8);
    *(uint64_t *)cdataptr(tokcd) = 0x1122334455667788ull;
    *(uint64_t *)cdataptr(lookcd) = 0x8877665544332211ull;
    setcdataV(L, &outer.tokval, tokcd);
    setcdataV(L, &inner.lookaheadval, lookcd);
    lj_gc_pubroot(L, &outer.tokval);
    lj_gc_pubroot(L, &inner.lookaheadval);
  }
#endif

  lj_gc2_mark_begin(g);
  lj_lex_gc2_markroots(g, tg);
  assert_raw_backing_marked(g, &outer);
  assert_raw_backing_marked(g, &inner);
#if LJ_HASFFI
  assert(lj_gc2_ismarked(g, gcV(&outer.tokval)) == 1);
  assert(lj_gc2_ismarked(g, gcV(&inner.lookaheadval)) == 1);
#endif
  lj_gc2_cycle_to_idle(g);

  lj_lex_cleanup(L, &inner);
  assert(lj_tg_lexstate_acq(tg) == &outer);
  lj_lex_cleanup(L, &outer);
  assert(lj_tg_lexstate_acq(tg) == NULL);
  /* Protected load cleanup and callers may both attempt terminal cleanup.
  ** A second call must remain a harmless no-op. */
  lj_lex_cleanup(L, &outer);
  assert(lj_tg_lexstate_acq(tg) == NULL);

  /* lex_setup publishes before its first reader call. Exercise a nonlocal
  ** reader throw from exactly that window and prove lua_loadx drains it. */
  lj_tab_read_enter(tg);
  {
    uint64_t outer_epoch = lj_tg_tab_read_epoch_acq(tg);
    status = lua_load(L, throw_reader, NULL, "=(reader-throw-outer-pin)");
    assert(status != LUA_OK);
    assert(lj_tg_tab_read_depth_acq(tg) == 1);
    assert(lj_tg_tab_read_epoch_acq(tg) == outer_epoch);
    assert(lj_tg_lexstate_acq(tg) == NULL);
    lua_settop(L, 0);
  }
  lj_tab_read_leave(tg);
  assert(lj_tg_tab_read_depth_acq(tg) == 0);

  status = lua_load(L, throw_reader, NULL, "=(reader-throw)");
  assert(status != LUA_OK);
  assert(lj_tg_lexstate_acq(tg) == NULL);
  lua_settop(L, 0);

  assert(luaL_loadstring(L, "return 6 * 7") == LUA_OK);
  assert(lj_tg_lexstate_acq(tg) == NULL);
  lua_pop(L, 1);
  lua_close(L);

  printf("t-parser-gc2-roots OK: exact raw roots, LIFO, and unwind verified\n");
  return 0;
}
