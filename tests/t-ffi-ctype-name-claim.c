/*
** Focused guard for M7 FFI ctype duplicate-name publication.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_ctype.h"

static void dostring(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "lua error: %s\n", err ? err : "(non-string)");
    assert(0);
  }
}

static CTypeID new_named(CTState *cts, lua_State *L, CTInfo info, CTSize size,
			 GCstr *name, CType **ctp)
{
  CTypeID id = lj_ctype_new_l(L, cts, ctp);
  CType *ct = *ctp;
  ct->info = info;
  ct->size = size;
  ct->sib = 0;
  ct->next = 0;
  ctype_setname(ct, name);
  return id;
}

static void force_table_move_after_reserve(lua_State *L, CTState *cts)
{
  CTypeTab *before = ctype_tabh_acq(cts);
  int guard = 0;
  while (ctype_tabh_acq(cts) == before) {
    CType *ct;
    CTypeID id = lj_ctype_new_l(L, cts, &ct);
    ct->info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD));
    ct->size = 0;
    ct->sib = 0;
    ct->next = 0;
    setgcrefnull(ct->name);
    assert(id != 0);
    assert(++guard < CTID_MAX);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  CTState *cts;
  GCstr *name;
  CType *ct1, *ct2, *ct3, *found;
  CTypeID id1, id2, id3, winner;
  const uint32_t default_ns = (1u << CT_TYPEDEF);
  const uint32_t struct_ns = (1u << CT_STRUCT);

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);

  dostring(L, "local ffi = require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);

  name = lj_str_newlit(L, "lj_m7_name_claim_t");

  id1 = new_named(cts, L, CTINFO(CT_TYPEDEF, CTID_INT32), 0, name, &ct1);
  winner = lj_ctype_addname_unique(cts, ct1, id1, default_ns);
  assert(winner == id1);
  assert(lj_ctype_getname(cts, &found, name, default_ns) == id1);
  assert(found == ctype_get(cts, id1));

  id2 = new_named(cts, L, CTINFO(CT_TYPEDEF, CTID_INT32), 0, name, &ct2);
  winner = lj_ctype_addname_unique(cts, ct2, id2, default_ns);
  assert(winner == id1);
  assert(ctype_isabandoned(ctype_get(cts, id2)->info));
  assert(lj_ctype_getname(cts, &found, name, default_ns) == id1);
  lua_pushinteger(L, (lua_Integer)id2);
  lua_setglobal(L, "lj_m7_name_claim_loser_id");
  dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.typeinfo(lj_m7_name_claim_loser_id) == nil,\n"
    "       'ffi.typeinfo exposed abandoned ctype')\n");

  id3 = new_named(cts, L, CTINFO(CT_STRUCT, CTALIGN(2)), 4, name, &ct3);
  force_table_move_after_reserve(L, cts);
  assert(ct3 != ctype_get(cts, id3));
  winner = lj_ctype_addname_unique(cts, ct3, id3, struct_ns);
  assert(winner == id3);
  assert(lj_ctype_getname(cts, &found, name, struct_ns) == id3);
  assert(lj_ctype_getname(cts, &found, name, default_ns) == id1);

  dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "struct lj_m7_parser_name_claim { double x; };\n"
    "typedef int lj_m7_parser_name_claim;\n"
    "enum lj_m7_parser_enum_a { LJ_M7_PARSER_DUP_CONST = 11 };\n"
    "]]\n"
    "assert(ffi.sizeof('struct lj_m7_parser_name_claim') == 8,\n"
    "       'parser struct tag namespace was shadowed')\n"
    "assert(ffi.sizeof('lj_m7_parser_name_claim') == 4,\n"
    "       'parser typedef namespace was shadowed')\n"
    "assert(not pcall(ffi.cdef,\n"
    "  'enum lj_m7_parser_enum_b { LJ_M7_PARSER_DUP_CONST = 12 };'),\n"
    "  'parser duplicate enum constant was accepted')\n"
    "assert(ffi.C.LJ_M7_PARSER_DUP_CONST == 11,\n"
    "       'parser duplicate enum loser replaced winner')\n");

  lua_close(L);
  printf("t-ffi-ctype-name-claim OK: duplicate names pick one winner and abandon losers\n");
  return 0;
}
