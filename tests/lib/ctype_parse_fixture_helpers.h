/*
** Shared ctype parse-token helpers for C fixtures.
*/

#ifndef TESTS_LIB_CTYPE_PARSE_FIXTURE_HELPERS_H
#define TESTS_LIB_CTYPE_PARSE_FIXTURE_HELPERS_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"

#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_trace.h"

static CTState *ljt_ctype_release_cts;
static uint32_t ljt_ctype_release_seq;
static uint32_t ljt_ctype_release_count;
static CTState *ljt_ctype_trace_cts;
static uint32_t ljt_ctype_trace_seq;
static uint32_t ljt_ctype_trace_start_count;
static uint32_t ljt_ctype_trace_abort_count;
static uint32_t ljt_ctype_trace_ctbusy_count;
static uint32_t ljt_ctype_trace_stop_count;

static inline uint32_t ljt_ctype_parse_seq(CTState *cts)
{
  uint32_t seq = la_load32_acq(&cts->parse_token);
  assert((seq & 1u) == 0);
  return seq;
}

static inline uint32_t ljt_ctype_hold_parse_token(CTState *cts)
{
  uint32_t seq = ljt_ctype_parse_seq(cts);
  ctype_parse_token_rel(cts, seq + 1u);
  assert((ctype_parse_token_acq(cts) & 1u) != 0);
  return seq + 2u;
}

static inline void ljt_ctype_release_parse_token(CTState *cts, uint32_t seq)
{
  ctype_parse_token_rel(cts, seq);
  (void)ctype_parse_token_wake(cts, 0x7fffffff);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

static inline void ljt_ctype_arm_release_hook(CTState *cts, uint32_t seq)
{
  ljt_ctype_release_cts = cts;
  ljt_ctype_release_seq = seq;
  ljt_ctype_release_count = 0;
}

static int ljt_ctype_release_parse_token_lua(lua_State *L)
{
  const char *what = lua_tostring(L, 1);
  if (ljt_ctype_release_cts && what && strcmp(what, "abort") == 0) {
    ljt_ctype_release_parse_token(ljt_ctype_release_cts,
				  ljt_ctype_release_seq);
    ljt_ctype_release_cts = NULL;
    ljt_ctype_release_seq = 0;
    ljt_ctype_release_count++;
  }
  return 0;
}

static int ljt_ctype_release_parse_token_count_lua(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ljt_ctype_release_count);
  return 1;
}

static inline void ljt_ctype_install_release_hook(lua_State *L)
{
  lua_pushcfunction(L, ljt_ctype_release_parse_token_lua);
  lua_setglobal(L, "lj_m7_release_parse_token");
  lua_pushcfunction(L, ljt_ctype_release_parse_token_count_lua);
  lua_setglobal(L, "lj_m7_release_parse_token_count");
}

static inline uint32_t ljt_ctype_hold_parse_token_for_abort(lua_State *L,
							    CTState *cts)
{
  uint32_t seq = ljt_ctype_hold_parse_token(cts);
  ljt_ctype_arm_release_hook(cts, seq);
  ljt_ctype_install_release_hook(L);
  return seq;
}

static inline void ljt_ctype_assert_abort_released(CTState *cts)
{
  assert(ljt_ctype_release_count != 0);
  assert(ljt_ctype_release_cts == NULL);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

static int ljt_ctype_trace_parse_token_lua(lua_State *L)
{
  const char *what = lua_tostring(L, 1);
  if (!ljt_ctype_trace_cts || !what)
    return 0;
  if (strcmp(what, "start") == 0 && ljt_ctype_trace_seq == 0) {
    ljt_ctype_trace_seq = ljt_ctype_hold_parse_token(ljt_ctype_trace_cts);
    ljt_ctype_trace_start_count++;
  } else if (strcmp(what, "abort") == 0 && ljt_ctype_trace_seq != 0) {
    int i, top = lua_gettop(L);
    if (top >= 5 && lua_isnumber(L, 5) &&
	lua_tointeger(L, 5) == (lua_Integer)LJ_TRERR_CTBUSY)
      ljt_ctype_trace_ctbusy_count++;
    for (i = 5; i <= top; i++) {
      const char *s = lua_tostring(L, i);
      if (s && strstr(s, "ctype parser busy"))
	ljt_ctype_trace_ctbusy_count++;
    }
    ljt_ctype_release_parse_token(ljt_ctype_trace_cts,
				  ljt_ctype_trace_seq);
    ljt_ctype_trace_seq = 0;
    ljt_ctype_trace_cts = NULL;
    ljt_ctype_trace_abort_count++;
  } else if (strcmp(what, "stop") == 0 && ljt_ctype_trace_seq != 0) {
    ljt_ctype_release_parse_token(ljt_ctype_trace_cts,
				  ljt_ctype_trace_seq);
    ljt_ctype_trace_seq = 0;
    ljt_ctype_trace_cts = NULL;
    ljt_ctype_trace_stop_count++;
  }
  return 0;
}

static int ljt_ctype_trace_abort_count_lua(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ljt_ctype_trace_abort_count);
  return 1;
}

static int ljt_ctype_trace_ctbusy_count_lua(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ljt_ctype_trace_ctbusy_count);
  return 1;
}

static int ljt_ctype_trace_start_count_lua(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ljt_ctype_trace_start_count);
  return 1;
}

static int ljt_ctype_trace_stop_count_lua(lua_State *L)
{
  lua_pushinteger(L, (lua_Integer)ljt_ctype_trace_stop_count);
  return 1;
}

static inline void ljt_ctype_arm_trace_abort(lua_State *L, CTState *cts)
{
  ljt_ctype_trace_cts = cts;
  ljt_ctype_trace_seq = 0;
  ljt_ctype_trace_start_count = 0;
  ljt_ctype_trace_abort_count = 0;
  ljt_ctype_trace_ctbusy_count = 0;
  ljt_ctype_trace_stop_count = 0;
  lua_pushcfunction(L, ljt_ctype_trace_parse_token_lua);
  lua_setglobal(L, "lj_m7_trace_parse_token");
  lua_pushcfunction(L, ljt_ctype_trace_start_count_lua);
  lua_setglobal(L, "lj_m7_trace_parse_token_start_count");
  lua_pushcfunction(L, ljt_ctype_trace_abort_count_lua);
  lua_setglobal(L, "lj_m7_trace_parse_token_abort_count");
  lua_pushcfunction(L, ljt_ctype_trace_ctbusy_count_lua);
  lua_setglobal(L, "lj_m7_trace_parse_token_ctbusy_count");
  lua_pushcfunction(L, ljt_ctype_trace_stop_count_lua);
  lua_setglobal(L, "lj_m7_trace_parse_token_stop_count");
}

static inline void ljt_ctype_assert_trace_abort_released(CTState *cts)
{
  assert(ljt_ctype_trace_abort_count != 0);
  assert(ljt_ctype_trace_cts == NULL);
  assert(ljt_ctype_trace_seq == 0);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

#endif
