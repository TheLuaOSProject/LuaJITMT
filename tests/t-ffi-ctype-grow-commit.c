/* Park a real CType table grow after it prepared its new storage. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t parser_stage, grow_stage;
static CTypeID target;
static MSize previous_capacity;
static unsigned park_at;
static __thread unsigned growing;

static void wait_for(uint32_t *stage, uint32_t value)
{
  while (la_load32_acq(stage) != value)
    sched_yield();
}

CType *__real_lj_ctype_publish(CTState *cts, CTypeID id, CType *src);
CType *__wrap_lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  if (id == target && ctype_size_acq(src) == 4 &&
      la_load32_acq(&parser_stage) == 0) {
    assert(ctype_top_acq(cts) == previous_capacity);
    la_store32_rel(&parser_stage, 1);
    wait_for(&parser_stage, 2);
  }
  return __real_lj_ctype_publish(cts, id, src);
}

int __real_lj_gc2_markmem(global_State *g, void *p);
int __wrap_lj_gc2_markmem(global_State *g, void *p)
{
  int marked = __real_lj_gc2_markmem(g, p);
  if (growing && --growing == 0) {
    la_store32_rel(&grow_stage, 1);
    wait_for(&grow_stage, 2);
  }
  return marked;
}

static int prepare_growth(lua_State *L)
{
  CTState *cts = ctype_ctsG(G(L));
  target = (CTypeID)luaL_checkinteger(L, 1);
  previous_capacity = ctype_tab_sizetab_acq(&cts->tab);
  while (ctype_top_acq(cts) < previous_capacity - 1) {
    CType *ct;
    lj_ctype_new_l(L, cts, &ct);
    ctype_info_rel(ct, CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD)));
  }
  return 0;
}

static int start_growth(lua_State *L)
{
  CTState *cts = ctype_ctsG(G(L));
  CTypeID id;
  growing = park_at;
  id = lj_ctype_intern_l(L, cts, CTINFO(CT_ARRAY, CTALIGN(2)|CTID_INT32),
			913 * sizeof(int32_t));
  assert(!growing);
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}

static int wait_parser(lua_State *L)
{
  UNUSED(L);
  wait_for(&parser_stage, 1);
  return 0;
}

static int release_parser(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&parser_stage, 2);
  return 0;
}

static int wait_growth(lua_State *L)
{
  UNUSED(L);
  wait_for(&grow_stage, 1);
  return 0;
}

static int release_growth(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&grow_stage, 2);
  return 0;
}

int main(int argc, char **argv)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  park_at = argc == 2 ? (unsigned)atoi(argv[1]) : 1;
  assert(park_at == 1 || park_at == 2);
  lua_register(L, "prepare_growth", prepare_growth);
  lua_register(L, "start_growth", start_growth);
  lua_register(L, "wait_parser", wait_parser);
  lua_register(L, "release_parser", release_parser);
  lua_register(L, "wait_growth", wait_growth);
  lua_register(L, "release_growth", release_growth);
  ljt_lua_dostring(L,
    "local ffi, th = require('ffi'), require('threading')\n"
    "if jit then jit.off() end\n"
    "ffi.cdef('struct grow_commit;')\n"
    "local ct = ffi.typeof('struct grow_commit')\n"
    "prepare_growth(tonumber(ct))\n"
    "local parser = th.spawn(function()\n"
    "  require('ffi').cdef('struct grow_commit { int value; };')\n"
    "end)\n"
    "wait_parser()\n"
    "local grower = th.spawn(start_growth)\n"
    "wait_growth()\n"
    "release_parser()\n"
    "assert(parser:join(10))\n"
    "assert(ffi.sizeof(ct) == 4 and ffi.offsetof(ct, 'value') == 0)\n"
    /* Grow through the Lua API while the first producer is paused either
    ** before installing its span or before publishing the larger capacity. */
    "local peer = ffi.typeof('int[914]')\n"
    "assert(ffi.sizeof(peer) == 3656 and ffi.new(peer)[0] == 0)\n"
    "release_growth()\n"
    "local joined, arrid = grower:join(10)\n"
    "assert(joined and tonumber(ffi.typeof('int[913]')) == arrid)\n"
    "assert(ffi.sizeof(ct) == 4 and ffi.offsetof(ct, 'value') == 0,\n"
    "       'CType growth reverted a committed struct layout')\n");
  lua_close(L);
  puts("t-ffi-ctype-grow-commit OK");
  return 0;
}
