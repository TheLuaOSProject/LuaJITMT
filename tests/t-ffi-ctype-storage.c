/* Grow through the last representable CTypeID, retaining earlier slots. */
#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lib/lua_fixture_helpers.h"

static GCSize fail_size;

void *__real_lj_mem_new_deferred_nothrow(lua_State *L, GCSize bytes);
void *__wrap_lj_mem_new_deferred_nothrow(lua_State *L, GCSize bytes)
{
  if (fail_size && bytes == fail_size) {
    fail_size = 0;
    return NULL;
  }
  return __real_lj_mem_new_deferred_nothrow(L, bytes);
}

static int reserve_one(lua_State *L)
{
  CTState *cts = ctype_ctsG(G(L));
  CType *ct;
  lj_ctype_new_l(L, cts, &ct);
  ctype_info_rel(ct, CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD)));
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  CType *initial, *held[CTYPE_TAB_SPANS];
  MSize i, j;
  ljt_lua_dostring(L,
    "local f = require('ffi'); f.cdef('struct keep { int n; };')\n"
    "local ct = f.typeof('struct keep'); local v = ct(73)\n"
    "function check_type() assert(v.n == 73 and f.sizeof(ct) == 4 "
    "and f.offsetof(ct, 'n') == 0) end\n");
  cts = ctype_ctsG(G(L));
  initial = ctype_get(cts, CTID_INT32);
  for (i = 0; i < CTYPE_TAB_SPANS; i++) {
    MSize capacity = CTYPE_TAB_MIN << i;
    while (ctype_top_acq(cts) < capacity) reserve_one(L);
    assert(ctype_top_acq(cts) == capacity);
    assert(ctype_tab_sizetab_acq(&cts->tab) == capacity);
    fail_size = capacity * sizeof(CType);
    lua_pushcfunction(L, reserve_one);
    assert(lua_pcall(L, 0, 0, 0) == LUA_ERRMEM && fail_size == 0);
    lua_pop(L, 1);
    assert(ctype_top_acq(cts) == capacity);
    assert(ctype_tab_span_acq(&cts->tab, i) == NULL);
    reserve_one(L);
    held[i] = ctype_get(cts, capacity);
    assert(ctype_tab_sizetab_acq(&cts->tab) == capacity * 2);
    lua_gc(L, LUA_GCCOLLECT, 0);
    assert(ctype_get(cts, CTID_INT32) == initial && ctype_size_acq(initial) == 4);
    for (j = 0; j <= i; j++) {
      assert(ctype_tab_span_acq(&cts->tab, j) == held[j]);
      assert(lj_gc2_mem_registered_known(G(L), held[j]));
      assert(ctype_isabandoned(ctype_info_acq(held[j])));
    }
    ljt_lua_dostring(L, "check_type()");
  }
  while (ctype_top_acq(cts) < CTID_MAX) reserve_one(L);
  lua_pushcfunction(L, reserve_one);
  assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  assert(ctype_top_acq(cts) == CTID_MAX);
  lua_close(L);
  puts("t-ffi-ctype-storage OK");
  return 0;
}
