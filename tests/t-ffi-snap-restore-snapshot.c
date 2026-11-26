/*
** Restore sunk cdata on native side exits while a real cdef is suspended.
*/

#include <assert.h>
#include <sched.h>
#include <stdio.h>

#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_snap.h"
#include "lib/lua_fixture_helpers.h"

enum { INACTIVE, PARSING, PAUSED, RELEASED };
static uint32_t stage;
static CTypeID target, struct_id, boxed_id;
static unsigned structs_restored, boxes_restored;
static _Thread_local unsigned restoring;

CType *__real_lj_ctype_publish(CTState *cts, CTypeID id, CType *src);
CType *__wrap_lj_ctype_publish(CTState *cts, CTypeID id, CType *src)
{
  CType *ct = __real_lj_ctype_publish(cts, id, src);
  if (la_load32_acq(&stage) == PARSING && id == target &&
      ctype_size_acq(ct) == sizeof(int)) {
    assert(ctype_parse_token_acq(cts) & 1u);
    la_store32_rel(&stage, PAUSED);
    while (la_load32_acq(&stage) == PAUSED)
      sched_yield();
  }
  return ct;
}

const BCIns *__real_lj_snap_restore_exit(jit_State *J, void *exptr,
  lua_State *L, GCtrace *T, TraceNo parent, ExitNo exitno);
const BCIns *__wrap_lj_snap_restore_exit(jit_State *J, void *exptr,
  lua_State *L, GCtrace *T, TraceNo parent, ExitNo exitno)
{
  const BCIns *pc;
  restoring++;
  pc = __real_lj_snap_restore_exit(J, exptr, L, T, parent, exitno);
  restoring--;
  return pc;
}

GCcdata *__real_lj_cdata_newx_l(lua_State *L, CTState *cts, CTypeID id,
                               CTSize sz, CTInfo info);
GCcdata *__wrap_lj_cdata_newx_l(lua_State *L, CTState *cts, CTypeID id,
                               CTSize sz, CTInfo info)
{
  GCcdata *cd = __real_lj_cdata_newx_l(L, cts, id, sz, info);
  if (restoring && la_load32_acq(&stage) == PAUSED) {
    assert(ctype_parse_token_acq(cts) & 1u);
    if (id == struct_id)
      structs_restored++;
    if (id == boxed_id)
      boxes_restored++;
  }
  return cd;
}

static int watch_types(lua_State *L)
{
  target = (CTypeID)luaL_checkinteger(L, 1);
  struct_id = (CTypeID)luaL_checkinteger(L, 2);
  boxed_id = (CTypeID)luaL_checkinteger(L, 3);
  la_store32_rel(&stage, PARSING);
  return 0;
}

static int wait_for_parser(lua_State *L)
{
  UNUSED(L);
  while (la_load32_acq(&stage) != PAUSED)
    sched_yield();
  return 0;
}

static int verify_and_release(lua_State *L)
{
  UNUSED(L);
  assert(la_load32_acq(&stage) == PAUSED);
  assert(structs_restored > 0);
  assert(boxes_restored > 0);
  la_store32_rel(&stage, RELEASED);
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_register(L, "watch_types", watch_types);
  lua_register(L, "wait_for_parser", wait_for_parser);
  lua_register(L, "verify_and_release", verify_and_release);
  ljt_lua_dostring(L,
    "local ffi, th = require('ffi'), require('threading')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; double y; } snap_ready_t;\n"
    "typedef const int64_t snap_boxed_t;\n"
    "struct snap_pending;\n"
    "]]\n"
    "local struct_t, boxed_t = ffi.typeof('snap_ready_t'), ffi.typeof('snap_boxed_t')\n"
    "local pending = ffi.typeof('struct snap_pending')\n"
    "watch_types(tonumber(pending), tonumber(ffi.typeof(struct_t())),\n"
    "            tonumber(ffi.typeof(boxed_t(0))))\n"
    "local function make_struct(n, stop)\n"
    "  for i = 1, n do\n"
    "    local obj = struct_t(i, i + 0.5)\n"
    "    if i == stop then return obj end\n"
    "  end\n"
    "end\n"
    "local function make_i64(n, stop)\n"
    "  for i = 1, n do\n"
    "    local v = boxed_t(9007199254740000 + i)\n"
    "    if i == stop then return v end\n"
    "  end\n"
    "end\n"
    "local start = th.channel(1)\n"
    "local w = th.spawn(function(start)\n"
    "  assert(start:recv())\n"
    "  require('ffi').cdef('struct snap_pending { int x; };')\n"
    "  return true\n"
    "end, start)\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1000')\n"
    "for _ = 1, 60 do\n"
    "  assert(make_struct(80, 0) == nil)\n"
    "  assert(make_i64(80, 0) == nil)\n"
    "end\n"
    "assert(start:send(true))\n"
    "wait_for_parser()\n"
    "assert(ffi.sizeof(pending) == nil)\n"
    "assert(ffi.sizeof(struct_t) == 16 and ffi.offsetof(struct_t, 'y') == 8)\n"
    "local obj, v = make_struct(80, 37), make_i64(80, 41)\n"
    "assert(ffi.istype(struct_t, obj) and obj.x == 37 and obj.y == 37.5)\n"
    "assert(type(v) == 'cdata' and ffi.istype(boxed_t, v))\n"
    "assert(v == 9007199254740041LL)\n"
    "verify_and_release()\n"
    "local joined, result = w:join(10)\n"
    "assert(joined and result, tostring(result))\n"
    "assert(ffi.sizeof(pending) == 4 and ffi.offsetof(pending, 'x') == 0)\n"
    "obj, v = make_struct(80, 43), make_i64(80, 47)\n"
    "assert(obj.x == 43 and obj.y == 43.5 and v == 9007199254740047LL)\n");
  lua_close(L);
  printf("t-ffi-snap-restore-snapshot OK: restored %u structs and %u boxes during cdef\n",
         structs_restored, boxes_restored);
  return 0;
}
