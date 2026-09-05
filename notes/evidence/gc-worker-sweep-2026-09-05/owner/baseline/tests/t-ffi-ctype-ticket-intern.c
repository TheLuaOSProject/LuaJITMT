/*
** Focused regression test for M7 FFI ctype ticket allocation and duplicate interning.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"

#include "lib/lua_fixture_helpers.h"

static void init_test_ctype(CType *ct, CTInfo info, CTSize size)
{
  ctype_info_rel(ct, info);
  ctype_size_rel(ct, size);
  ctype_sib_rel(ct, 0);
  ctype_next_rel(ct, 0);
  ctype_clearname(ct);
}

static void force_table_move_after_reserve(lua_State *L, CTState *cts)
{
  CTypeTab *before = ctype_tabh_acq(cts);
  int guard = 0;
  while (ctype_tabh_acq(cts) == before) {
    CType *ct;
    CTypeID id = lj_ctype_new_l(L, cts, &ct);
    init_test_ctype(ct, CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD)), 0);
    assert(id != 0);
    assert(++guard < CTID_MAX);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  CTState *cts;
  CTypeID top0, top1, baseid, movedid, ptrid1, ptrid2;
  CType *basect, *movedct, *curct, *ptrct;
  CTInfo ptrinfo;
  int isnew = 0;

  g = G(L);

  ljt_lua_dostring(L, "local ffi = require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);

  top0 = ctype_top_acq(cts);
  baseid = lj_ctype_new_l(L, cts, &basect);
  assert(baseid == top0);
  init_test_ctype(basect, CTINFO(CT_STRUCT, CTALIGN(2)), 4);
  assert(ctype_top_acq(cts) == top0 + 1u);

  movedid = lj_ctype_new_l(L, cts, &movedct);
  force_table_move_after_reserve(L, cts);
  assert(movedct != ctype_get(cts, movedid));
  init_test_ctype(movedct, CTINFO(CT_STRUCT, CTALIGN(2)), 8);
  curct = lj_ctype_publish(cts, movedid, movedct);
  assert(curct == ctype_get(cts, movedid));
  assert(curct != movedct);
  assert(ctype_info_acq(curct) == CTINFO(CT_STRUCT, CTALIGN(2)));
  assert(ctype_size_acq(curct) == 8);

  ptrinfo = CTINFO(CT_PTR, CTALIGN_PTR|movedid);
  ptrid1 = lj_ctype_intern_new_l(L, cts, ptrinfo, CTSIZE_PTR, &isnew);
  assert(isnew == 1);
  assert(ptrid1 > movedid);
  top1 = ctype_top_acq(cts);
  assert(top1 == ptrid1 + 1u);

  isnew = 1;
  ptrid2 = lj_ctype_intern_new_l(L, cts, ptrinfo, CTSIZE_PTR, &isnew);
  assert(isnew == 0);
  assert(ptrid2 == ptrid1);
  assert(ctype_top_acq(cts) == top1);

  ptrct = ctype_get(cts, ptrid1);
  assert(ctype_info_acq(ptrct) == ptrinfo);
  assert(ctype_size_acq(ptrct) == CTSIZE_PTR);
  assert(!ctype_isabandoned(ctype_info_acq(ptrct)));

  lua_close(L);
  printf("t-ffi-ctype-ticket-intern OK: ctype tickets are monotonic, RCU-published, and duplicate interning reuses IDs\n");
  return 0;
}
