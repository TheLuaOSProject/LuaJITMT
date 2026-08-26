/*
** ARM64 fast pcall/xpcall root-anchor checkpoint regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_frame.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

#if !LJ_TARGET_ARM64 || !LJ_FR2 || !LJ_FRAME_PCALL_ROOT_ANCHOR
#error "this fixture requires ARM64/FR2 fast-pcall root-anchor checkpoints"
#endif

static uint32_t ambient_top;

static void check_packed_frame_accessors(void)
{
  TValue frame;
  uint64_t word = ((uint64_t)UINT32_C(0x12345678) << 32) |
                  (uint64_t)(24 + FRAME_PCALLH);
  frame.ftsz = (int64_t)word;
  assert(frame_typep(&frame) == FRAME_PCALLH);
  assert(frame_pcall_root_top(&frame) == UINT32_C(0x12345678));
  assert(frame_delta(&frame) == 3);
  assert(frame_sized(&frame) == 24);
}

/* Publish one otherwise weakly held value, then abandon the exact anchor on a
** nonlocal edge. The surrounding Lua fast pcall/xpcall must roll back to the
** nonzero ambient checkpoint before it exposes the caught error. */
static int anchor_throw_c(lua_State *L)
{
  TGState *tg = G2TG(G(L));
  TValue *anchor;
  uint32_t anchoridx;
  int mode = (int)luaL_checkinteger(L, 3);

  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  assert(lj_tg_root_anchor_top_acq(tg) == ambient_top);
  anchor = lj_tg_root_anchor_push(L, tg, L->base, &anchoridx);
  assert(anchor != NULL && tvistab(anchor));
  assert(lj_tg_root_anchor_top_acq(tg) == ambient_top + 1u);

  lua_pushnil(L);
  lua_replace(L, 1);  /* The weak value now survives only through the anchor. */
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushinteger(L, 1);
  lua_rawget(L, 2);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);
  lua_settop(L, 0);

  UNUSED(anchoridx);  /* Deliberately abandoned for protected-frame rollback. */
  if (mode == 0)
    lj_safepoint_checkstop(L, LJ_GC2_HS_STOPREQ);
  else if (mode == 1)
    lj_err_mem(L);
  else
    lj_err_msg(L, LJ_ERR_TABOV);
  abort();
  return 0;
}

static int anchor_depth_c(lua_State *L)
{
  lua_pushinteger(L,
    (lua_Integer)lj_tg_root_anchor_top_acq(G2TG(G(L))));
  return 1;
}

/* Keep two sentinel tables alive only through nested TG anchors while a Lua
** callback executes all fast-protected-call paths. The outer C protected call
** returns success, so it cannot mask a leaked or over-rolled inner checkpoint. */
static int ambient_call_c(lua_State *L)
{
  TGState *tg = G2TG(G(L));
  TValue *anchor0, *anchor1;
  uint32_t idx0, idx1;
  uint32_t top0 = lj_tg_root_anchor_top_acq(tg);
  int status;

  luaL_checktype(L, 1, LUA_TFUNCTION);
  luaL_checktype(L, 2, LUA_TTABLE);
  luaL_checktype(L, 3, LUA_TTABLE);
  luaL_checktype(L, 4, LUA_TTABLE);  /* Weak-value witness table. */

  anchor0 = lj_tg_root_anchor_push(L, tg, L->base + 1, &idx0);
  assert(anchor0 != NULL && tvistab(anchor0));
  anchor1 = lj_tg_root_anchor_push(L, tg, L->base + 2, &idx1);
  assert(anchor1 != NULL && tvistab(anchor1));
  ambient_top = top0 + 2u;
  assert(lj_tg_root_anchor_top_acq(tg) == ambient_top);

  lua_pushnil(L);
  lua_replace(L, 2);
  lua_pushnil(L);
  lua_replace(L, 3);  /* Sentinels are now strong only in the two anchors. */

  lua_pushvalue(L, 1);
  status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ambient pcall callback failed: %s\n",
            lua_tostring(L, -1));
    abort();
  }
  assert(lj_tg_root_anchor_top_acq(tg) == ambient_top);
  anchor0 = lj_tg_root_anchor_slot_acq(tg, idx0);
  anchor1 = lj_tg_root_anchor_slot_acq(tg, idx1);
  assert(anchor0 != NULL && tvistab(anchor0));
  assert(anchor1 != NULL && tvistab(anchor1));

  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushinteger(L, 1);
  lua_rawget(L, 4);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);
  lua_pushinteger(L, 2);
  lua_rawget(L, 4);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);

  lj_tg_root_anchor_pop(tg, idx1);
  lj_tg_root_anchor_pop(tg, idx0);
  assert(lj_tg_root_anchor_top_acq(tg) == top0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushinteger(L, 1);
  lua_rawget(L, 4);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_pushinteger(L, 2);
  lua_rawget(L, 4);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  lua_settop(L, 0);
  return 0;
}

static void push_weak_outer_arguments(lua_State *L, const char *callback)
{
  int base = lua_gettop(L) + 1;

  lua_pushcfunction(L, ambient_call_c);
  assert(luaL_loadstring(L, callback) == LUA_OK);
  lua_newtable(L);  /* First sentinel. */
  lua_newtable(L);  /* Second sentinel. */
  lua_newtable(L);  /* Weak-value witness. */
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, -2));

  lua_pushinteger(L, 1);
  lua_pushvalue(L, base + 2);
  lua_rawset(L, base + 4);
  lua_pushinteger(L, 2);
  lua_pushvalue(L, base + 3);
  lua_rawset(L, base + 4);
  assert(lua_pcall(L, 4, 0, 0) == LUA_OK);
  assert(lua_gettop(L) == base - 1);
}

int main(void)
{
  static const char callback[] =
    "local depth, throw = anchor_depth, anchor_throw\n"
    "local ambient = depth()\n"
    "assert(ambient >= 2)\n"
    "local ok, a, b = pcall(function(x, y) return x+y, x*y end, 6, 7)\n"
    "assert(ok and a == 13 and b == 42 and depth() == ambient)\n"
    "ok, a, b = xpcall(function(x, y) return x-y, x+y end, tostring, 9, 4)\n"
    "assert(ok and a == 5 and b == 13 and depth() == ambient)\n"
    "local calls = 0\n"
    "local obj = setmetatable({}, { __tostring = function()\n"
    "  calls = calls + 1; collectgarbage('collect')\n"
    "  assert(depth() == ambient); return 'tail-' .. calls\n"
    "end })\n"
    "ok, a = pcall(tostring, obj)\n"
    "assert(ok and a == 'tail-1' and depth() == ambient)\n"
    "ok, a = xpcall(tostring, function(e) return e end, obj)\n"
    "assert(ok and a == 'tail-2' and depth() == ambient)\n"
    "for mode = 0, 2 do\n"
    "  local weak = setmetatable({}, { __mode = 'v' })\n"
    "  weak[1] = { mode = mode }\n"
    "  local outer_ok, inner_ok, err, caught = pcall(function()\n"
    "    local inner, why\n"
    "    if mode == 1 then\n"
    "      inner, why = xpcall(throw, tostring, weak[1], weak, mode)\n"
    "    else\n"
    "      inner, why = pcall(throw, weak[1], weak, mode)\n"
    "    end\n"
    "    return inner, why, depth()\n"
    "  end)\n"
    "  assert(outer_ok and not inner_ok and type(err) == 'string')\n"
    "  assert(caught == ambient and depth() == ambient)\n"
    "  collectgarbage('collect'); assert(weak[1] == nil)\n"
    "end\n";
  lua_State *L;
  TGState *tg;
  uint32_t top0;

  check_packed_frame_accessors();
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  tg = G2TG(G(L));
  top0 = lj_tg_root_anchor_top_acq(tg);

  lua_pushcfunction(L, anchor_throw_c);
  lua_setglobal(L, "anchor_throw");
  lua_pushcfunction(L, anchor_depth_c);
  lua_setglobal(L, "anchor_depth");
  assert(luaL_dostring(L,
    "assert(jit.status() == false and jit.opt == nil)\n") == LUA_OK);

  push_weak_outer_arguments(L, callback);
  assert(lj_tg_root_anchor_top_acq(tg) == top0);

  lua_pushnil(L);
  lua_setglobal(L, "anchor_throw");
  lua_pushnil(L);
  lua_setglobal(L, "anchor_depth");
  lua_close(L);
  puts("t-arm64-pcall-anchor OK: packed fast-protected frames and exact unwind");
  return 0;
}
