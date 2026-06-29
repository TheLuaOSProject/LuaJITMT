/*
** Focused guard for public FFI ctype metadata snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
  int saw_native;
} ParseReleaseCtx;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000l;
  ts.tv_nsec = ns % 1000000000l;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *release_parse_token(void *arg)
{
  ParseReleaseCtx *ctx = (ParseReleaseCtx *)arg;
  int spins;
  for (spins = 0; spins < 1000; spins++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      ctx->saw_native = 1;
      break;
    }
    sleep_ns(1000000);
  }
  ljt_ctype_release_parse_token(ctx->cts, ctx->release_seq);
  return NULL;
}

static void assert_direct_name_waits_without_lock(lua_State *L, CTState *cts,
						  TGState *tg)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local cqarr2 = ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag[2][3]')\n"
    "assert(tonumber(cqarr2) == lj_m7_typeinfo_tag_const_arr2_id)\n"
    "assert(ffi.sizeof('const struct lj_m7_typeinfo_snapshot_tag[2][3]') == 24)\n"
    "local tagarr2 = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2][3]')\n"
    "assert(tonumber(tagarr2) == lj_m7_typeinfo_tag_arr2_id)\n"
    "assert(ffi.sizeof('struct lj_m7_typeinfo_snapshot_tag[2][3]') == 24)\n"
    "local tagarr = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2]')\n"
    "assert(tonumber(tagarr) == lj_m7_typeinfo_tag_arr_id)\n"
    "assert(ffi.sizeof('struct lj_m7_typeinfo_snapshot_tag[2]') == 8)\n"
    "local pptag = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag **')\n"
    "assert(tonumber(pptag) == lj_m7_typeinfo_tag_ptrptr_id)\n"
    "assert(ffi.sizeof('struct lj_m7_typeinfo_snapshot_tag **') == "
    "ffi.sizeof('void *'))\n"
    "local int_ptr9 = 'int' .. string.rep(' *', 9)\n"
    "assert(tonumber(ffi.typeof(int_ptr9)) == lj_m7_typeinfo_int_ptr9_id)\n"
    "assert(ffi.sizeof(int_ptr9) == ffi.sizeof('void *'))\n"
    "local tag_ptr9 = 'struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep(' *', 9)\n"
    "assert(tonumber(ffi.typeof(tag_ptr9)) == lj_m7_typeinfo_tag_ptr9_id)\n"
    "assert(ffi.sizeof(tag_ptr9) == ffi.sizeof('void *'))\n"
    "local int_arr9 = 'int' .. string.rep('[1]', 9)\n"
    "assert(tonumber(ffi.typeof(int_arr9)) == lj_m7_typeinfo_int_arr9_id)\n"
    "assert(ffi.sizeof(int_arr9) == 4)\n"
    "local tag_arr9 = 'struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep('[1]', 9)\n"
    "assert(tonumber(ffi.typeof(tag_arr9)) == lj_m7_typeinfo_tag_arr9_id)\n"
    "assert(ffi.sizeof(tag_arr9) == 4)\n"
    "local ct = ffi.typeof('lj_m7_typeinfo_snapshot_t')\n"
    "assert(tonumber(ct) == lj_m7_typeinfo_snapshot_id)\n"
    "assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "local tag = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag')\n"
    "assert(tonumber(tag) == lj_m7_typeinfo_tag_id)\n"
    "assert(ffi.sizeof('union lj_m7_typeinfo_snapshot_union') == 8)\n"
    "assert(ffi.sizeof('enum lj_m7_typeinfo_snapshot_enum') == 4)\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

static void assert_deep_suffixes_without_lock(lua_State *L, CTState *cts)
{
  uint32_t seq = ljt_ctype_parse_seq(cts);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local int_ptr9 = 'int' .. string.rep(' *', 9)\n"
    "assert(tonumber(ffi.typeof(int_ptr9)) == lj_m7_typeinfo_int_ptr9_id)\n"
    "assert(ffi.sizeof(int_ptr9) == ffi.sizeof('void *'))\n");
  assert(ljt_ctype_parse_seq(cts) == seq);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local tag_ptr9 = 'struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep(' *', 9)\n"
    "assert(tonumber(ffi.typeof(tag_ptr9)) == lj_m7_typeinfo_tag_ptr9_id)\n"
    "assert(ffi.sizeof(tag_ptr9) == ffi.sizeof('void *'))\n");
  assert(ljt_ctype_parse_seq(cts) == seq);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local int_arr9 = 'int' .. string.rep('[1]', 9)\n"
    "assert(tonumber(ffi.typeof(int_arr9)) == lj_m7_typeinfo_int_arr9_id)\n"
    "assert(ffi.sizeof(int_arr9) == 4)\n"
    "assert(ffi.alignof(int_arr9) == 4)\n");
  assert(ljt_ctype_parse_seq(cts) == seq);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local tag_arr9 = 'struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep('[1]', 9)\n"
    "assert(tonumber(ffi.typeof(tag_arr9)) == lj_m7_typeinfo_tag_arr9_id)\n"
    "assert(ffi.sizeof(tag_arr9) == 4)\n");
  assert(ljt_ctype_parse_seq(cts) == seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_typeinfo_snapshot_t;\n"
    "typedef struct lj_m7_typeinfo_snapshot_tag { int x; } "
    "lj_m7_typeinfo_snapshot_tag_t;\n"
    "typedef union lj_m7_typeinfo_snapshot_union { int x; double y; } "
    "lj_m7_typeinfo_snapshot_union_t;\n"
    "typedef enum lj_m7_typeinfo_snapshot_enum { "
    "LJ_M7_TYPEINFO_SNAPSHOT_E = 7 } lj_m7_typeinfo_snapshot_enum_t;\n"
    "typedef const int lj_m7_typeinfo_const_int_alias_t;\n"
    "typedef const struct lj_m7_typeinfo_snapshot_tag "
    "lj_m7_typeinfo_const_tag_alias_t;\n"
    "typedef long long lj_m7_typeinfo_longlong_alias_t;\n"
    "typedef long double lj_m7_typeinfo_longdouble_alias_t;\n"
    "]]\n"
    "lj_m7_typeinfo_snapshot_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t'))\n"
    "lj_m7_typeinfo_snapshot_const_id = "
    "tonumber(ffi.typeof('const lj_m7_typeinfo_snapshot_t'))\n"
    "lj_m7_typeinfo_snapshot_ptr_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t *'))\n"
    "lj_m7_typeinfo_snapshot_const_ptr_id = "
    "tonumber(ffi.typeof('const lj_m7_typeinfo_snapshot_t *'))\n"
    "lj_m7_typeinfo_snapshot_ptr_const_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t * const'))\n"
    "lj_m7_typeinfo_snapshot_ptrptr_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t **'))\n"
    "lj_m7_typeinfo_snapshot_arr_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t[3]'))\n"
    "lj_m7_typeinfo_snapshot_ptr_arr_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t *[2]'))\n"
    "lj_m7_typeinfo_snapshot_arr2_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t[2][3]'))\n"
    "lj_m7_typeinfo_snapshot_ptr_arr2_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t *[2][3]'))\n"
    "lj_m7_typeinfo_tag_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag'))\n"
    "lj_m7_typeinfo_tag_const_id = "
    "tonumber(ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag'))\n"
    "lj_m7_typeinfo_tag_ptr_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag *'))\n"
    "lj_m7_typeinfo_tag_const_ptr_id = "
    "tonumber(ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag *'))\n"
    "lj_m7_typeinfo_tag_ptr_const_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag * const'))\n"
    "lj_m7_typeinfo_tag_ptr_const_ptr_volatile_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag * const * volatile'))\n"
    "lj_m7_typeinfo_tag_ptrptr_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag **'))\n"
    "lj_m7_typeinfo_tag_ptr9_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep(' *', 9)))\n"
    "lj_m7_typeinfo_tag_arr_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2]'))\n"
    "lj_m7_typeinfo_tag_arr2_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2][3]'))\n"
    "lj_m7_typeinfo_tag_arr9_id = "
    "tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag' .. "
    "string.rep('[1]', 9)))\n"
    "lj_m7_typeinfo_tag_const_arr2_id = "
    "tonumber(ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag[2][3]'))\n"
    "lj_m7_typeinfo_union_id = "
    "tonumber(ffi.typeof('union lj_m7_typeinfo_snapshot_union'))\n"
    "lj_m7_typeinfo_union_ptr_id = "
    "tonumber(ffi.typeof('union lj_m7_typeinfo_snapshot_union *'))\n"
    "lj_m7_typeinfo_union_ptrptr_id = "
    "tonumber(ffi.typeof('union lj_m7_typeinfo_snapshot_union **'))\n"
    "lj_m7_typeinfo_union_arr_id = "
    "tonumber(ffi.typeof('union lj_m7_typeinfo_snapshot_union[2]'))\n"
    "lj_m7_typeinfo_enum_id = "
    "tonumber(ffi.typeof('enum lj_m7_typeinfo_snapshot_enum'))\n"
    "lj_m7_typeinfo_enum_const_id = "
    "tonumber(ffi.typeof('const enum lj_m7_typeinfo_snapshot_enum'))\n"
    "lj_m7_typeinfo_enum_ptr_id = "
    "tonumber(ffi.typeof('enum lj_m7_typeinfo_snapshot_enum *'))\n"
    "lj_m7_typeinfo_enum_ptrptr_id = "
    "tonumber(ffi.typeof('enum lj_m7_typeinfo_snapshot_enum **'))\n"
    "lj_m7_typeinfo_int8_id = tonumber(ffi.typeof('int8_t'))\n"
    "lj_m7_typeinfo_uint8_id = tonumber(ffi.typeof('uint8_t'))\n"
    "lj_m7_typeinfo_int16_id = tonumber(ffi.typeof('int16_t'))\n"
    "lj_m7_typeinfo_uint16_id = tonumber(ffi.typeof('uint16_t'))\n"
    "lj_m7_typeinfo_uint32_id = tonumber(ffi.typeof('uint32_t'))\n"
    "lj_m7_typeinfo_int_id = tonumber(ffi.typeof('int'))\n"
    "lj_m7_typeinfo_const_int_id = tonumber(ffi.typeof('const int'))\n"
    "lj_m7_typeinfo_const_int_alias_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_const_int_alias_t'))\n"
    "lj_m7_typeinfo_volatile_int_id = tonumber(ffi.typeof('volatile int'))\n"
    "lj_m7_typeinfo_cv_int_id = tonumber(ffi.typeof('const volatile int'))\n"
    "lj_m7_typeinfo_int_ptr_id = tonumber(ffi.typeof('int *'))\n"
    "lj_m7_typeinfo_const_int_ptr_id = tonumber(ffi.typeof('const int *'))\n"
    "lj_m7_typeinfo_int_ptr_const_id = tonumber(ffi.typeof('int * const'))\n"
    "lj_m7_typeinfo_int_ptr_cv_id = tonumber(ffi.typeof('int * const volatile'))\n"
    "lj_m7_typeinfo_int_ptr_const_ptr_volatile_id = "
    "tonumber(ffi.typeof('int * const * volatile'))\n"
    "lj_m7_typeinfo_int_restrict_id = tonumber(ffi.typeof('int restrict'))\n"
    "lj_m7_typeinfo_int_ptr_restrict_id = tonumber(ffi.typeof('int * restrict'))\n"
    "lj_m7_typeinfo_int_ptr_restrict_ptr_id = "
    "tonumber(ffi.typeof('int * restrict *'))\n"
    "lj_m7_typeinfo_int_ptr_restrict_arr_id = "
    "tonumber(ffi.typeof('int * restrict[2]'))\n"
    "lj_m7_typeinfo_const_int_ptr_const_id = "
    "tonumber(ffi.typeof('const int * const'))\n"
    "lj_m7_typeinfo_int_ptrptr_id = tonumber(ffi.typeof('int **'))\n"
    "lj_m7_typeinfo_int_ptr9_id = "
    "tonumber(ffi.typeof('int' .. string.rep(' *', 9)))\n"
    "lj_m7_typeinfo_int_arr_id = tonumber(ffi.typeof('int[4]'))\n"
    "lj_m7_typeinfo_int_arr2_id = tonumber(ffi.typeof('int[2][3]'))\n"
    "lj_m7_typeinfo_int_arr9_id = "
    "tonumber(ffi.typeof('int' .. string.rep('[1]', 9)))\n"
    "lj_m7_typeinfo_const_int_arr2_id = tonumber(ffi.typeof('const int[2][3]'))\n"
    "lj_m7_typeinfo_longlong_id = tonumber(ffi.typeof('long long'))\n"
    "lj_m7_typeinfo_longlong_alias_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_longlong_alias_t'))\n"
    "lj_m7_typeinfo_ulonglong_id = tonumber(ffi.typeof('unsigned long long'))\n"
    "lj_m7_typeinfo_longlong_ptr_id = tonumber(ffi.typeof('long long *'))\n"
    "lj_m7_typeinfo_longlong_arr_id = tonumber(ffi.typeof('long long[2]'))\n"
    "lj_m7_typeinfo_long_id = tonumber(ffi.typeof('long'))\n"
    "lj_m7_typeinfo_ulong_id = tonumber(ffi.typeof('unsigned long'))\n"
    "lj_m7_typeinfo_int64kw_id = tonumber(ffi.typeof('__int64'))\n"
    "lj_m7_typeinfo_uint64kw_id = tonumber(ffi.typeof('unsigned __int64'))\n"
    "lj_m7_typeinfo_int64kw_ptr_id = tonumber(ffi.typeof('__int64 *'))\n"
    "lj_m7_typeinfo_int128kw_id = tonumber(ffi.typeof('__int128'))\n"
    "lj_m7_typeinfo_uint128kw_id = tonumber(ffi.typeof('unsigned __int128'))\n"
    "lj_m7_typeinfo_int128kw_arr_id = tonumber(ffi.typeof('__int128[2]'))\n"
    "lj_m7_typeinfo_longdouble_id = tonumber(ffi.typeof('long double'))\n"
    "lj_m7_typeinfo_longdouble_alias_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_longdouble_alias_t'))\n"
    "lj_m7_typeinfo_const_longdouble_id = "
    "tonumber(ffi.typeof('const long double'))\n"
    "lj_m7_typeinfo_longdouble_ptr_id = tonumber(ffi.typeof('long double *'))\n"
    "lj_m7_typeinfo_complex_id = tonumber(ffi.typeof('complex'))\n"
    "lj_m7_typeinfo_complex_float_id = tonumber(ffi.typeof('complex float'))\n"
    "lj_m7_typeinfo_const_complex_id = tonumber(ffi.typeof('const complex'))\n"
    "lj_m7_typeinfo_complex_ptr_id = tonumber(ffi.typeof('complex *'))\n"
    "lj_m7_typeinfo_complex_arr_id = tonumber(ffi.typeof('complex[2]'))\n"
    "lj_m7_typeinfo_cvoid_id = tonumber(ffi.typeof('const void'))\n"
    "lj_m7_typeinfo_cvoid_ptrptr_id = tonumber(ffi.typeof('const void **'))\n"
    "lj_m7_typeinfo_cchar_id = tonumber(ffi.typeof('const char'))\n"
    "lj_m7_typeinfo_cchar_ptrptr_id = tonumber(ffi.typeof('const char **'))\n"
    "lj_m7_typeinfo_cchar_arr_id = tonumber(ffi.typeof('const char[4]'))\n"
    "lj_m7_typeinfo_cchar_arr2_id = tonumber(ffi.typeof('const char[2][3]'))\n"
    "lj_m7_typeinfo_const_tag_alias_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_const_tag_alias_t'))\n"
    "assert(lj_m7_typeinfo_const_int_alias_id == "
    "lj_m7_typeinfo_const_int_id)\n"
    "assert(lj_m7_typeinfo_const_tag_alias_id == "
    "lj_m7_typeinfo_tag_const_id)\n"
    "assert(lj_m7_typeinfo_int_restrict_id == lj_m7_typeinfo_int_id)\n"
    "assert(lj_m7_typeinfo_int_ptr_restrict_id == lj_m7_typeinfo_int_ptr_id)\n"
    "assert(lj_m7_typeinfo_longlong_alias_id == lj_m7_typeinfo_longlong_id)\n"
    "assert(lj_m7_typeinfo_longdouble_alias_id == lj_m7_typeinfo_longdouble_id)\n"
    "assert(type(lj_m7_typeinfo_snapshot_id) == 'number')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "for i = 1, 100 do\n"
    "  local ct = ffi.typeof('lj_m7_typeinfo_snapshot_t')\n"
    "  assert(tonumber(ct) == lj_m7_typeinfo_snapshot_id)\n"
    "  assert(tonumber(ffi.typeof('const lj_m7_typeinfo_snapshot_t')) == "
    "lj_m7_typeinfo_snapshot_const_id)\n"
    "  assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t const')) == "
    "lj_m7_typeinfo_snapshot_const_id)\n"
    "  local pct = ffi.typeof('lj_m7_typeinfo_snapshot_t *')\n"
    "  assert(tonumber(pct) == lj_m7_typeinfo_snapshot_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('const lj_m7_typeinfo_snapshot_t *')) == "
    "lj_m7_typeinfo_snapshot_const_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t * const')) == "
    "lj_m7_typeinfo_snapshot_ptr_const_id)\n"
    "  assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t *const')) == "
    "lj_m7_typeinfo_snapshot_ptr_const_id)\n"
    "  local ppct = ffi.typeof('lj_m7_typeinfo_snapshot_t **')\n"
    "  assert(tonumber(ppct) == lj_m7_typeinfo_snapshot_ptrptr_id)\n"
    "  assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t* *')) == "
    "lj_m7_typeinfo_snapshot_ptrptr_id)\n"
    "  local arr = ffi.typeof('lj_m7_typeinfo_snapshot_t[3]')\n"
    "  assert(tonumber(arr) == lj_m7_typeinfo_snapshot_arr_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t[3]') == 12)\n"
    "  local parr = ffi.typeof('lj_m7_typeinfo_snapshot_t *[2]')\n"
    "  assert(tonumber(parr) == lj_m7_typeinfo_snapshot_ptr_arr_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t *[2]') == "
    "2 * ffi.sizeof('void *'))\n"
    "  local arr2 = ffi.typeof('lj_m7_typeinfo_snapshot_t[2][3]')\n"
    "  assert(tonumber(arr2) == lj_m7_typeinfo_snapshot_arr2_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t[2][3]') == 24)\n"
    "  local parr2 = ffi.typeof('lj_m7_typeinfo_snapshot_t *[2][3]')\n"
    "  assert(tonumber(parr2) == lj_m7_typeinfo_snapshot_ptr_arr2_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t *[2][3]') == "
    "6 * ffi.sizeof('void *'))\n"
    "  assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t*')) == "
    "lj_m7_typeinfo_snapshot_ptr_id)\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t *') == "
    "ffi.sizeof('void *'))\n"
    "  assert(ffi.alignof('lj_m7_typeinfo_snapshot_t *') == "
    "ffi.alignof('void *'))\n"
    "  assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "  assert(ffi.alignof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "  local tag = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag')\n"
    "  assert(tonumber(tag) == lj_m7_typeinfo_tag_id)\n"
    "  assert(tonumber(ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag')) == "
    "lj_m7_typeinfo_tag_const_id)\n"
    "  assert(tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag const')) == "
    "lj_m7_typeinfo_tag_const_id)\n"
    "  local ptag = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag *')\n"
    "  assert(tonumber(ptag) == lj_m7_typeinfo_tag_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag *')) == "
    "lj_m7_typeinfo_tag_const_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag * const')) == "
    "lj_m7_typeinfo_tag_ptr_const_id)\n"
    "  assert(tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag * const * volatile')) == "
    "lj_m7_typeinfo_tag_ptr_const_ptr_volatile_id)\n"
    "  local pptag = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag **')\n"
    "  assert(tonumber(pptag) == lj_m7_typeinfo_tag_ptrptr_id)\n"
    "  local tagarr = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2]')\n"
    "  assert(tonumber(tagarr) == lj_m7_typeinfo_tag_arr_id)\n"
    "  local tagarr2 = ffi.typeof('struct lj_m7_typeinfo_snapshot_tag[2][3]')\n"
    "  assert(tonumber(tagarr2) == lj_m7_typeinfo_tag_arr2_id)\n"
    "  local qtagarr2 = ffi.typeof('const struct lj_m7_typeinfo_snapshot_tag[2][3]')\n"
    "  assert(tonumber(qtagarr2) == lj_m7_typeinfo_tag_const_arr2_id)\n"
    "  assert(tonumber(ffi.typeof('struct lj_m7_typeinfo_snapshot_tag*')) == "
    "lj_m7_typeinfo_tag_ptr_id)\n"
    "  assert(ffi.sizeof('struct lj_m7_typeinfo_snapshot_tag') == 4)\n"
    "  assert(ffi.alignof('struct lj_m7_typeinfo_snapshot_tag') == 4)\n"
    "  local u = ffi.typeof('union lj_m7_typeinfo_snapshot_union')\n"
    "  assert(tonumber(u) == lj_m7_typeinfo_union_id)\n"
    "  local pu = ffi.typeof('union lj_m7_typeinfo_snapshot_union *')\n"
    "  assert(tonumber(pu) == lj_m7_typeinfo_union_ptr_id)\n"
    "  local ppu = ffi.typeof('union lj_m7_typeinfo_snapshot_union **')\n"
    "  assert(tonumber(ppu) == lj_m7_typeinfo_union_ptrptr_id)\n"
    "  local uarr = ffi.typeof('union lj_m7_typeinfo_snapshot_union[2]')\n"
    "  assert(tonumber(uarr) == lj_m7_typeinfo_union_arr_id)\n"
    "  assert(ffi.sizeof('union lj_m7_typeinfo_snapshot_union') == 8)\n"
    "  local e = ffi.typeof('enum lj_m7_typeinfo_snapshot_enum')\n"
    "  assert(tonumber(e) == lj_m7_typeinfo_enum_id)\n"
    "  assert(tonumber(ffi.typeof('const enum lj_m7_typeinfo_snapshot_enum')) == "
    "lj_m7_typeinfo_enum_const_id)\n"
    "  local pe = ffi.typeof('enum lj_m7_typeinfo_snapshot_enum *')\n"
    "  assert(tonumber(pe) == lj_m7_typeinfo_enum_ptr_id)\n"
    "  local ppe = ffi.typeof('enum lj_m7_typeinfo_snapshot_enum **')\n"
    "  assert(tonumber(ppe) == lj_m7_typeinfo_enum_ptrptr_id)\n"
    "  assert(ffi.sizeof('enum lj_m7_typeinfo_snapshot_enum') == 4)\n"
    "  assert(tonumber(ffi.typeof('int *')) == lj_m7_typeinfo_int_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('const int')) == lj_m7_typeinfo_const_int_id)\n"
    "  assert(tonumber(ffi.typeof('int const')) == lj_m7_typeinfo_const_int_id)\n"
    "  assert(tonumber(ffi.typeof('__const int')) == lj_m7_typeinfo_const_int_id)\n"
    "  assert(tonumber(ffi.typeof('volatile int')) == lj_m7_typeinfo_volatile_int_id)\n"
    "  assert(tonumber(ffi.typeof('int volatile')) == lj_m7_typeinfo_volatile_int_id)\n"
    "  assert(tonumber(ffi.typeof('const volatile int')) == lj_m7_typeinfo_cv_int_id)\n"
    "  assert(tonumber(ffi.typeof('volatile const int')) == lj_m7_typeinfo_cv_int_id)\n"
    "  assert(tonumber(ffi.typeof('const int *')) == lj_m7_typeinfo_const_int_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('int const *')) == lj_m7_typeinfo_const_int_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('int * const')) == lj_m7_typeinfo_int_ptr_const_id)\n"
    "  assert(tonumber(ffi.typeof('int *const')) == lj_m7_typeinfo_int_ptr_const_id)\n"
    "  assert(tonumber(ffi.typeof('int * const volatile')) == lj_m7_typeinfo_int_ptr_cv_id)\n"
    "  assert(tonumber(ffi.typeof('int * const * volatile')) == "
    "lj_m7_typeinfo_int_ptr_const_ptr_volatile_id)\n"
    "  assert(tonumber(ffi.typeof('int restrict')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('restrict int')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('__extension__ int')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('int * restrict')) == "
    "lj_m7_typeinfo_int_ptr_restrict_id)\n"
    "  assert(tonumber(ffi.typeof('int *__restrict')) == "
    "lj_m7_typeinfo_int_ptr_restrict_id)\n"
    "  assert(tonumber(ffi.typeof('int * __restrict__')) == "
    "lj_m7_typeinfo_int_ptr_restrict_id)\n"
    "  assert(tonumber(ffi.typeof('int * restrict *')) == "
    "lj_m7_typeinfo_int_ptr_restrict_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('int * restrict[2]')) == "
    "lj_m7_typeinfo_int_ptr_restrict_arr_id)\n"
    "  assert(tonumber(ffi.typeof('const int * const')) == "
    "lj_m7_typeinfo_const_int_ptr_const_id)\n"
    "  assert(tonumber(ffi.typeof('int **')) == lj_m7_typeinfo_int_ptrptr_id)\n"
    "  assert(tonumber(ffi.typeof('int[4]')) == lj_m7_typeinfo_int_arr_id)\n"
    "  assert(tonumber(ffi.typeof('int [ 4 ]')) == "
    "lj_m7_typeinfo_int_arr_id)\n"
    "  assert(ffi.sizeof('int[4]') == 16)\n"
    "  assert(ffi.alignof('int[4]') == 4)\n"
    "  assert(tonumber(ffi.typeof('int[2][3]')) == lj_m7_typeinfo_int_arr2_id)\n"
    "  assert(tonumber(ffi.typeof('int [ 2 ] [ 3 ]')) == "
    "lj_m7_typeinfo_int_arr2_id)\n"
    "  assert(ffi.sizeof('int[2][3]') == 24)\n"
    "  assert(ffi.alignof('int[2][3]') == 4)\n"
    "  assert(tonumber(ffi.typeof('const int[2][3]')) == "
    "lj_m7_typeinfo_const_int_arr2_id)\n"
    "  assert(tonumber(ffi.typeof('int const[2][3]')) == "
    "lj_m7_typeinfo_const_int_arr2_id)\n"
    "  assert(tonumber(ffi.typeof('long long')) == lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('long long int')) == lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('signed long long')) == lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('signed long long int')) == "
    "lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned long long')) == "
    "lj_m7_typeinfo_ulonglong_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned long long int')) == "
    "lj_m7_typeinfo_ulonglong_id)\n"
    "  assert(tonumber(ffi.typeof('long long *')) == lj_m7_typeinfo_longlong_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('long long[2]')) == lj_m7_typeinfo_longlong_arr_id)\n"
    "  assert(ffi.sizeof('long long[2]') == 16)\n"
    "  assert(tonumber(ffi.typeof('__signed')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('__signed__')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('__signed int')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('int __signed__')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('__signed char')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('char __signed')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('char unsigned')) == lj_m7_typeinfo_uint8_id)\n"
    "  assert(tonumber(ffi.typeof('__signed short')) == lj_m7_typeinfo_int16_id)\n"
    "  assert(tonumber(ffi.typeof('short __signed int')) == "
    "lj_m7_typeinfo_int16_id)\n"
    "  assert(tonumber(ffi.typeof('short unsigned')) == lj_m7_typeinfo_uint16_id)\n"
    "  assert(tonumber(ffi.typeof('int unsigned')) == lj_m7_typeinfo_uint32_id)\n"
    "  assert(tonumber(ffi.typeof('__signed long')) == lj_m7_typeinfo_long_id)\n"
    "  assert(tonumber(ffi.typeof('long __signed int')) == lj_m7_typeinfo_long_id)\n"
    "  assert(tonumber(ffi.typeof('long unsigned')) == lj_m7_typeinfo_ulong_id)\n"
    "  assert(tonumber(ffi.typeof('__signed long long')) == "
    "lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('long long __signed int')) == "
    "lj_m7_typeinfo_longlong_id)\n"
    "  assert(tonumber(ffi.typeof('long long unsigned')) == "
    "lj_m7_typeinfo_ulonglong_id)\n"
    "  assert(tonumber(ffi.typeof('__int8')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('signed __int8')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('__signed __int8')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('__int8 signed')) == lj_m7_typeinfo_int8_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned __int8')) == lj_m7_typeinfo_uint8_id)\n"
    "  assert(tonumber(ffi.typeof('__int8 unsigned')) == lj_m7_typeinfo_uint8_id)\n"
    "  assert(tonumber(ffi.typeof('__int16')) == lj_m7_typeinfo_int16_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned __int16')) == lj_m7_typeinfo_uint16_id)\n"
    "  assert(tonumber(ffi.typeof('__int32')) == lj_m7_typeinfo_int_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned __int32')) == lj_m7_typeinfo_uint32_id)\n"
    "  assert(tonumber(ffi.typeof('__int64')) == lj_m7_typeinfo_int64kw_id)\n"
    "  assert(tonumber(ffi.typeof('signed __int64')) == lj_m7_typeinfo_int64kw_id)\n"
    "  assert(tonumber(ffi.typeof('__signed__ __int64')) == "
    "lj_m7_typeinfo_int64kw_id)\n"
    "  assert(tonumber(ffi.typeof('__int64 signed')) == lj_m7_typeinfo_int64kw_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned __int64')) == "
    "lj_m7_typeinfo_uint64kw_id)\n"
    "  assert(tonumber(ffi.typeof('__int64 unsigned')) == "
    "lj_m7_typeinfo_uint64kw_id)\n"
    "  assert(tonumber(ffi.typeof('__int64 *')) == lj_m7_typeinfo_int64kw_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('__int128')) == lj_m7_typeinfo_int128kw_id)\n"
    "  assert(tonumber(ffi.typeof('unsigned __int128')) == "
    "lj_m7_typeinfo_uint128kw_id)\n"
    "  assert(tonumber(ffi.typeof('__int128[2]')) == "
    "lj_m7_typeinfo_int128kw_arr_id)\n"
    "  assert(ffi.sizeof('__int128[2]') == 32)\n"
    "  assert(tonumber(ffi.typeof('long double')) == lj_m7_typeinfo_longdouble_id)\n"
    "  assert(tonumber(ffi.typeof('const long double')) == "
    "lj_m7_typeinfo_const_longdouble_id)\n"
    "  assert(tonumber(ffi.typeof('long double *')) == "
    "lj_m7_typeinfo_longdouble_ptr_id)\n"
    "  assert(ffi.sizeof('long double') == "
    "ffi.sizeof('lj_m7_typeinfo_longdouble_alias_t'))\n"
    "  assert(tonumber(ffi.typeof('complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('__complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('__complex__')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('complex double')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('double complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex double')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('double _Complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('__complex double')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('double __complex')) == lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('complex long double')) == "
    "lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('long double complex')) == "
    "lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex long double')) == "
    "lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('long double _Complex')) == "
    "lj_m7_typeinfo_complex_id)\n"
    "  assert(tonumber(ffi.typeof('complex float')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('float complex')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex float')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('float _Complex')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('__complex float')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('float __complex')) == "
    "lj_m7_typeinfo_complex_float_id)\n"
    "  assert(tonumber(ffi.typeof('const complex')) == "
    "lj_m7_typeinfo_const_complex_id)\n"
    "  assert(tonumber(ffi.typeof('complex const')) == "
    "lj_m7_typeinfo_const_complex_id)\n"
    "  assert(tonumber(ffi.typeof('complex *')) == lj_m7_typeinfo_complex_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex *')) == lj_m7_typeinfo_complex_ptr_id)\n"
    "  assert(tonumber(ffi.typeof('complex[2]')) == lj_m7_typeinfo_complex_arr_id)\n"
    "  assert(tonumber(ffi.typeof('_Complex[2]')) == lj_m7_typeinfo_complex_arr_id)\n"
    "  assert(ffi.sizeof('complex[2]') == 32)\n"
    "  assert(tonumber(ffi.typeof('const void')) == lj_m7_typeinfo_cvoid_id)\n"
    "  assert(tonumber(ffi.typeof('void const')) == lj_m7_typeinfo_cvoid_id)\n"
    "  assert(tonumber(ffi.typeof('const void **')) == "
    "lj_m7_typeinfo_cvoid_ptrptr_id)\n"
    "  assert(tonumber(ffi.typeof('const char')) == lj_m7_typeinfo_cchar_id)\n"
    "  assert(tonumber(ffi.typeof('char const')) == lj_m7_typeinfo_cchar_id)\n"
    "  assert(tonumber(ffi.typeof('const char **')) == "
    "lj_m7_typeinfo_cchar_ptrptr_id)\n"
    "  assert(tonumber(ffi.typeof('const char[4]')) == "
    "lj_m7_typeinfo_cchar_arr_id)\n"
    "  assert(tonumber(ffi.typeof('const char[2][3]')) == "
    "lj_m7_typeinfo_cchar_arr2_id)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_deep_suffixes_without_lock(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_direct_name_waits_without_lock(L, cts, tg);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  {
    uint32_t release_seq = ljt_ctype_hold_parse_token(cts);
    ljt_lua_dostring(L,
      "local ffi = require('ffi')\n"
      "assert(tonumber(ffi.typeof('int')) == lj_m7_typeinfo_int_id)\n"
      "assert(ffi.sizeof('int') == 4)\n"
      "assert(ffi.alignof('int') == 4)\n");
    assert((ctype_parse_token_acq(cts) & 1u) != 0);
    ljt_ctype_release_parse_token(cts, release_seq);
  }

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "assert(ffi.alignof('lj_m7_typeinfo_snapshot_t') == 4)\n"
    "assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t')) == "
    "lj_m7_typeinfo_snapshot_id)\n");
  assert(ljt_ctype_parse_seq(cts) == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef int lj_m7_typeinfo_snapshot_seq_t;')\n"
    "lj_m7_typeinfo_snapshot_seq_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_seq_t'))\n");
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 != seq1);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('lj_m7_typeinfo_snapshot_seq_t') == 4)\n"
    "assert(tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_seq_t')) == "
    "lj_m7_typeinfo_snapshot_seq_id)\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-typeinfo-snapshot OK: stable public ctype metadata reads avoid parser locking\n");
  return 0;
}
