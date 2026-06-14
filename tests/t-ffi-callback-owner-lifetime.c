/*
** Focused guard for FFI callback owner lifetime on worker detach.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"

typedef int (*OwnerCallback)(int);

static lua_State *mainL;
static CTState *saved_cts;
static OwnerCallback saved_cb;
static MSize saved_slot;
static lua_State *saved_owner;

static lua_State *slot_owner(void)
{
  lua_State **owner = (lua_State **)la_loadptr_acq(
    (void *const *)&saved_cts->cb.owner);
  assert(owner != NULL);
  return (lua_State *)la_loadptr_acq((void *const *)&owner[saved_slot]);
}

static void capture_cb(OwnerCallback cb)
{
  CTypeID1 *cbid;
  saved_cts = ctype_cts(mainL);
  saved_cb = cb;
  saved_slot = lj_ccallback_ptr2slot(saved_cts, (void *)cb);
  assert(saved_slot != ~0u);
  assert(saved_slot < la_load32_acq(&saved_cts->cb.sizeid));
  cbid = (CTypeID1 *)la_loadptr_acq((void *const *)&saved_cts->cb.cbid);
  assert(cbid != NULL);
  assert(la_load16_acq(&cbid[saved_slot]) != 0);
  saved_owner = slot_owner();
  assert(saved_owner != NULL);
  assert(saved_owner != mainL);
}

static void dostring(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "lua error: %s\n", err ? err : "(non-string)");
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  mainL = L;

  lua_pushlightuserdata(L, (void *)capture_cb);
  lua_setglobal(L, "lj_m7_capture_cb");

  dostring(L,
    "local threading = require('threading')\n"
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_m7_owner_cb_t)(int);\n"
    "typedef void (*lj_m7_capture_cb_t)(lj_m7_owner_cb_t);\n"
    "]]\n"
    "local out = threading.channel(1)\n"
    "local release = threading.channel(1)\n"
    "local worker = threading.spawn(function(out_ch, release_ch)\n"
    "  local ffi = require('ffi')\n"
    "  ffi.cdef[[typedef int (*lj_m7_owner_cb_t)(int);]]\n"
    "  local cb = ffi.cast('lj_m7_owner_cb_t', function(x)\n"
    "    return x + 37\n"
    "  end)\n"
    "  assert(out_ch:send(cb) == true)\n"
    "  local token, ok = release_ch:recv(10)\n"
    "  assert(ok == true and token == 'done')\n"
    "  return true\n"
    "end, out, release)\n"
    "local cb, ok = out:recv(10)\n"
    "assert(ok == true)\n"
    "local capture = ffi.cast('lj_m7_capture_cb_t', lj_m7_capture_cb)\n"
    "capture(cb)\n"
    "m7_owner_keep_cb = cb\n"
    "assert(release:send('done') == true)\n"
    "local joined = { worker:join() }\n"
    "assert(joined[1] == true)\n");

  assert(saved_cb != NULL);
  assert(slot_owner() == NULL);
  assert(saved_cb(5) == 42);

  dostring(L,
    "m7_owner_keep_cb:free()\n"
    "m7_owner_keep_cb = nil\n"
    "collectgarbage('collect')\n");

  lua_close(L);
  printf("t-ffi-callback-owner-lifetime OK: worker-owned callback disowned on detach\n");
  return 0;
}
