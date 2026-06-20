/*
** Focused guard for M7 FFI ctype ticket allocation and duplicate interning.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"

#include "lib/lua_fixture_helpers.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  CTState *cts;
  CTypeID top0, top1, baseid, ptrid1, ptrid2;
  CType *basect, *ptrct;
  CTInfo ptrinfo;
  int isnew = 0;

  g = G(L);

  ljt_lua_dostring(L, "local ffi = require('ffi')");
  cts = ctype_ctsG(g);
  assert(cts != NULL);

  top0 = ctype_top_acq(cts);
  baseid = lj_ctype_new_l(L, cts, &basect);
  assert(baseid == top0);
  basect->info = CTINFO(CT_STRUCT, CTALIGN(2));
  basect->size = 4;
  basect->sib = 0;
  basect->next = 0;
  setgcrefnull(basect->name);
  assert(ctype_top_acq(cts) == top0 + 1u);

  ptrinfo = CTINFO(CT_PTR, CTALIGN_PTR|baseid);
  ptrid1 = lj_ctype_intern_new_l(L, cts, ptrinfo, CTSIZE_PTR, &isnew);
  assert(isnew == 1);
  assert(ptrid1 == top0 + 1u);
  top1 = ctype_top_acq(cts);
  assert(top1 == top0 + 2u);

  isnew = 1;
  ptrid2 = lj_ctype_intern_new_l(L, cts, ptrinfo, CTSIZE_PTR, &isnew);
  assert(isnew == 0);
  assert(ptrid2 == ptrid1);
  assert(ctype_top_acq(cts) == top1);

  ptrct = ctype_get(cts, ptrid1);
  assert(ptrct->info == ptrinfo);
  assert(ptrct->size == CTSIZE_PTR);
  assert(!ctype_isabandoned(ptrct->info));

  lua_close(L);
  printf("t-ffi-ctype-ticket-intern OK: ctype tickets are monotonic and duplicate interning reuses IDs\n");
  return 0;
}
