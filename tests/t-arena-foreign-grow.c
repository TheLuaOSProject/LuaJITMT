/* Moving an owned raw allocation must use the actor performing the grow. */
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static int make_blocks(lua_State *L)
{
  void *small = lj_mem_new(L, 64u);
  void *huge = lj_mem_new(L, 32u * 1024u);
  memset(small, 0x5a, 64u);
  memset(huge, 0x31, 32u * 1024u);
  lua_pushlightuserdata(L, small);
  lua_pushlightuserdata(L, huge);
  return 2;
}

static int grow_block(lua_State *L)
{
  global_State *g = G(L);
  unsigned char *old = lua_touserdata(L, 1), *next;
  GCSize osize = (GCSize)luaL_checkinteger(L, 2);
  GCSize nsize = (GCSize)luaL_checkinteger(L, 3);
  int byte = luaL_checkinteger(L, 4);
  LJGC2Lease lease;
  GCSize i;
  assert(old && lj_arena_owner_acq(lj_arena_of(old)) != lj_tg_tid_acq(L2TG(L)));
  assert(lj_gc2_storage_lease_try(g, old, osize, &lease, NULL));
  if (osize > LJ_HUGE_THRESHOLD) {
    GCSize logical = 64u;
    assert(lj_mem_realloc(L, old, osize, logical) == old);
    osize = logical;
  }
  next = lj_mem_realloc(L, old, osize, nsize);
  assert(next != old);
  assert(lj_arena_owner_acq(lj_arena_of(next)) == lj_tg_tid_acq(L2TG(L)));
  for (i = 0; i < osize; i++) {
    assert(old[i] == byte);  /* The original reader is still admitted. */
    assert(next[i] == byte);
  }
  lj_gc2_lease_release(&lease);
  lj_mem_free(g, next, nsize);
  return 0;
}

static int check_stack_owner(lua_State *L)
{
  lua_State *co = lua_tothread(L, 1);
  GCArena *a;
  assert(co);
  a = lj_arena_of(tvref(co->stack));
  lua_pushboolean(L, lj_arena_owner_acq(a) == lj_tg_tid_acq(L2TG(L)));
  return 1;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  lua_register(L, "make_blocks", make_blocks);
  lua_register(L, "grow_block", grow_block);
  lua_register(L, "check_stack_owner", check_stack_owner);
  status = luaL_dostring(L,
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "local parked, unpark = th.channel(1), th.channel(1)\n"
    "local sentinel = assert(th.spawn(function()\n"
    "  assert(parked:send(true, 10))\n"
    "  assert(unpark:recv(10))\n"
    "end))\n"
    "assert(parked:recv(10))\n"
    "local ready, stop = th.channel(1), th.channel(1)\n"
    "local w = assert(th.spawn(function()\n"
    "  local p, h = make_blocks()\n"
    "  local c = coroutine.create(function()\n"
    "    local function grow(n)\n"
    "      if n == 0 then return 0 end\n"
    "      return 1 + grow(n - 1)\n"
    "    end\n"
    "    coroutine.yield(true)\n"
    "    return grow(110)\n"
    "  end)\n"
    "  assert(coroutine.resume(c))\n"
    "  assert(ready:send({p, h}, 10))\n"
    "  assert(stop:recv(10))\n"
    "  return c\n"
    "end))\n"
    "local p, ok = ready:recv(10)\n"
    "assert(ok)\n"
    "grow_block(p[1], 64, 4096, 0x5a)\n"
    "grow_block(p[2], 32768, 8192, 0x31)\n"
    "assert(stop:send(true, 10))\n"
    "local joined, c = w:join(10)\n"
    "assert(joined, c)\n"
    "assert(not check_stack_owner(c))\n"
    "local resumed, n = coroutine.resume(c)\n"
    "assert(resumed and n == 110, n)\n"
    "assert(check_stack_owner(c))\n"
    "assert(w:join(10))\n"
    "assert(unpark:send(true, 10))\n"
    "assert(sentinel:join(10))\n"
    "collectgarbage('restart')\n"
    "collectgarbage('collect')\n");
  if (status != 0)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == 0);
  lua_close(L);
  puts("foreign raw allocation growth passed");
  return 0;
}
