/* Suspend an actual cdef after its provisional definition becomes visible
** to the parser. Public layout readers must still see the previous type. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_ctype.h"
#include "lj_gc.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t stage, target;
static uint32_t callback_stage;

CType *__real_lj_ctype_publish(CTState *cts, CTypeID id, CType *src);
CType *__wrap_lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  CType *ct = __real_lj_ctype_publish(cts, id, src);
  if (id == la_load32_acq(&target) && ctype_size_acq(ct) == 4 &&
      la_load32_acq(&stage) == 0) {
    la_store32_rel(&stage, 1);
    while (la_load32_acq(&stage) == 1)
      sched_yield();
  }
  return ct;
}

void __real_lj_gc_pubroot(lua_State *L, cTValue *tv);
void __wrap_lj_gc_pubroot(lua_State *L, cTValue *tv)
{
  __real_lj_gc_pubroot(L, tv);
  if (la_load32_acq(&callback_stage) == 1 && tvisthread(tv)) {
    CTState *cts = ctype_ctsG(G(L));
    lua_State **owners = ctype_cb_owner_acq(cts);
    CTypeID1 *ids = ctype_cb_cbid_acq(cts);
    MSize slot, size = ctype_cb_sizeid_acq(cts);
    for (slot = 0; slot < size; slot++) {
      if (ctype_cb_owner_slot_acq(owners, slot) == threadV(tv) &&
	  ctype_cb_cbid_slot_acq(ids, slot) == 0) {
	/* The carrier is published. The callback's function and signature
	** will be installed only after this real slot reservation returns. */
	la_store32_rel(&callback_stage, 2);
	while (la_load32_acq(&callback_stage) == 2)
	  sched_yield();
	break;
      }
    }
  }
}

static int arm_callback(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&callback_stage, 1);
  return 0;
}

static int wait_for_callback(lua_State *L)
{
  UNUSED(L);
  while (la_load32_acq(&callback_stage) < 2)
    sched_yield();
  return 0;
}

static int release_callback(lua_State *L)
{
  UNUSED(L);
  assert(la_load32_acq(&callback_stage) == 2);
  la_store32_rel(&callback_stage, 3);
  return 0;
}

static int wait_for_parser(lua_State *L)
{
  UNUSED(L);
  while (!la_load32_acq(&stage)) sched_yield();
  return 0;
}

static int release_parser(lua_State *L)
{
  UNUSED(L);
  assert(la_load32_acq(&stage) == 1);
  la_store32_rel(&stage, 2);
  return 0;
}

static int watch_type(lua_State *L)
{
  la_store32_rel(&target, (uint32_t)luaL_checkinteger(L, 1));
  la_store32_rel(&stage, 0);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  lua_register(L, "wait_for_parser", wait_for_parser);
  lua_register(L, "release_parser", release_parser);
  lua_register(L, "watch_type", watch_type);
  lua_register(L, "arm_callback", arm_callback);
  lua_register(L, "wait_for_callback", wait_for_callback);
  lua_register(L, "release_callback", release_callback);
  ljt_lua_dostring(L,
    "local ffi, th = require('ffi'), require('threading')\n"
    "if jit then jit.off() end\n"
    "ffi.cdef('struct layout_ready { char a; double b; }; enum { LAYOUT_CONST = 123 }; int abs(int); typedef int (*layout_callback_t)(int);')\n"
    "local ready = ffi.typeof('struct layout_ready')\n"
    "local callback_type = ffi.typeof('layout_callback_t')\n"
    "for n = 1, 2 do\n"
    "  local name = 'struct layout_pending_'..n\n"
    "  ffi.cdef(name..';')\n"
    "  local pending = ffi.typeof(name)\n"
    "  watch_type(tonumber(pending))\n"
    "  local src = name..' { int x; };'..(n == 1 and '@' or '')\n"
    "  local w = th.spawn(function(src)\n"
    "    local ok = pcall(require('ffi').cdef, src); return ok\n"
    "  end, src)\n"
    "  wait_for_parser()\n"
    "  assert(ffi.C.abs(-7) == 7)\n"
    "  assert(ffi.C.LAYOUT_CONST == 123)\n"
    "  assert(ffi.sizeof(ready) == 16 and ffi.alignof(ready) == 8)\n"
    "  assert(ffi.offsetof(ready, 'b') == 8)\n"
    "  assert(ffi.sizeof('struct layout_ready') == 16)\n"
    "  assert(ffi.alignof('struct layout_ready') == 8)\n"
    "  assert(ffi.offsetof('struct layout_ready', 'b') == 8)\n"
    "  assert(ffi.sizeof(name) == nil and ffi.offsetof(name, 'x') == nil)\n"
    "  assert(ffi.sizeof(pending) == nil)\n"
    "  assert(ffi.offsetof(pending, 'x') == nil)\n"
    "  local cb = ffi.cast(callback_type, function(x) return x + 1 end)\n"
    "  assert(cb(41) == 42)\n"
    "  cb:set(function(x) return x + 2 end)\n"
    "  assert(cb(40) == 42)\n"
    "  cb:free()\n"
    "  assert(ffi.sizeof(ffi.new(ready)) == 16)\n"
    "  release_parser()\n"
    "  local joined, committed = w:join(10)\n"
    "  assert(joined and committed == (n == 2))\n"
    "  if committed then\n"
    "    assert(ffi.sizeof(pending) == 4 and ffi.offsetof(pending, 'x') == 0)\n"
    "  else assert(ffi.sizeof(pending) == nil) end\n"
    "  collectgarbage()\n"
    "end\n"
    "local w = th.spawn(function(ct)\n"
    "  arm_callback()\n"
    "  local cb = require('ffi').cast(ct, function(x) return x + 3 end)\n"
    "  local n = cb(40); cb:free(); return n\n"
    "end, callback_type)\n"
    "wait_for_callback()\n"
    "local cb = ffi.cast(callback_type, function(x) return x + 2 end)\n"
    "assert(cb(40) == 42 and ffi.sizeof(ffi.new(ready)) == 16)\n"
    "cb:free()\n"
    "release_callback()\n"
    "local joined, n = w:join(10)\n"
    "assert(joined and n == 43)\n");
  lua_close(L);
  puts("t-ffi-layout-parser-progress OK");
  return 0;
}
