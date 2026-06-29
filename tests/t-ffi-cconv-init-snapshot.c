/*
** Focused guard for lock-free aggregate conversion/init snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_cconv.h"
#include "lj_ctype.h"
#include "lj_str.h"
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

static void assert_cconv_init_waits_without_lock(lua_State *L, CTState *cts,
						 TGState *tg,
						 const char *chunk)
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
  ljt_lua_dostring(L, chunk);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

static void assert_predefined_init_avoids_wait(lua_State *L, CTState *cts)
{
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(tonumber(ffi.new(lj_m7_cconv_init_int_ct, 33)) == 33)\n"
    "assert(tonumber(ffi.new(lj_m7_cconv_init_double_ct, 1.5)) == 1.5)\n");

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

static CTypeID ctype_id_from_global(lua_State *L, const char *name)
{
  CTypeID id;
  lua_getglobal(L, name);
  assert(tviscdata(L->top - 1));
  assert(cdataV(L->top - 1)->ctypeid == CTID_CTYPEID);
  id = *(CTypeID *)cdataptr(cdataV(L->top - 1));
  L->top--;
  return id;
}

static CTypeID raw_ctype_snapshot_from_global(lua_State *L, CTState *cts,
					      const char *name, CType *snap)
{
  CTypeID id = ctype_id_from_global(L, name);
  CTypeID rid = ctype_rawid(cts, id);
  assert(lj_ctype_snapshot(cts, rid, snap) > 0);
  return rid;
}

static void assert_string_array_avoids_wait(lua_State *L, CTState *cts,
					    CTypeID did, CType *d)
{
  TValue tv;
  uint8_t dst[8];
  uint32_t seq0 = ljt_ctype_parse_seq(cts);
  uint32_t release_seq = ljt_ctype_hold_parse_token(cts);

  setstrV(L, &tv, lj_str_newz(L, "pre"));
  memset(dst, 0xaa, sizeof(dst));
  lj_cconv_ct_tv_l(L, cts, d, did, dst, &tv, 0);
  assert(memcmp(dst, "pre\0", 4) == 0);

  ljt_ctype_release_parse_token(cts, release_seq);
  assert(ljt_ctype_parse_seq(cts) == seq0 + 2u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  CType predef_str_arr_snap, parser_str_arr_snap;
  CTypeID predef_str_arr_id, parser_str_arr_id;
  uint32_t seq0, seq1, seq2, seq3, seq4, seq5, seq6, seq7;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_CCONV_INIT_A = 11, "
    "LJ_M7_CCONV_INIT_B = 13, LJ_M7_CCONV_INIT_C = 17 } "
    "lj_m7_cconv_init_e;\n"
    "typedef struct { lj_m7_cconv_init_e a; "
    "lj_m7_cconv_init_e b; } lj_m7_cconv_init_pair_t;\n"
    "typedef char lj_m7_cconv_init_char_t;\n"
    "typedef lj_m7_cconv_init_char_t lj_m7_cconv_init_str_arr_t[8];\n"
    "]]\n"
    "lj_m7_cconv_init_arr_ct = ffi.typeof('lj_m7_cconv_init_e[3]')\n"
    "lj_m7_cconv_init_pair_ct = ffi.typeof('lj_m7_cconv_init_pair_t')\n"
    "lj_m7_cconv_init_int_ct = ffi.typeof('int')\n"
    "lj_m7_cconv_init_double_ct = ffi.typeof('double')\n"
    "lj_m7_cconv_init_char_arr_ct = ffi.typeof('char[8]')\n"
    "lj_m7_cconv_init_str_arr_ct = ffi.typeof('lj_m7_cconv_init_str_arr_t')\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  predef_str_arr_id = raw_ctype_snapshot_from_global(L, cts,
    "lj_m7_cconv_init_char_arr_ct", &predef_str_arr_snap);
  parser_str_arr_id = raw_ctype_snapshot_from_global(L, cts,
    "lj_m7_cconv_init_str_arr_ct", &parser_str_arr_snap);
  seq0 = ljt_ctype_parse_seq(cts);

  assert_predefined_init_avoids_wait(L, cts);
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_string_array_avoids_wait(L, cts, predef_str_arr_id,
				  &predef_str_arr_snap);
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  assert_string_array_avoids_wait(L, cts, parser_str_arr_id,
				  &parser_str_arr_snap);
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2 + 2u);

  assert_cconv_init_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local a = ffi.new(lj_m7_cconv_init_arr_ct, {\n"
    "  'LJ_M7_CCONV_INIT_A', 'LJ_M7_CCONV_INIT_B',\n"
    "  'LJ_M7_CCONV_INIT_C' })\n"
    "assert(tonumber(a[0]) == 11)\n"
    "assert(tonumber(a[1]) == 13)\n"
    "assert(tonumber(a[2]) == 17)\n");
  seq4 = ljt_ctype_parse_seq(cts);
  assert(seq4 == seq3 + 2u);

  assert_cconv_init_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local p = ffi.new(lj_m7_cconv_init_pair_ct, {\n"
    "  a = 'LJ_M7_CCONV_INIT_A', b = 'LJ_M7_CCONV_INIT_B' })\n"
    "assert(tonumber(p.a) == 11)\n"
    "assert(tonumber(p.b) == 13)\n");
  seq5 = ljt_ctype_parse_seq(cts);
  assert(seq5 == seq4 + 2u);

  assert_cconv_init_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local a = ffi.new(lj_m7_cconv_init_arr_ct,\n"
    "  'LJ_M7_CCONV_INIT_A', 'LJ_M7_CCONV_INIT_B',\n"
    "  'LJ_M7_CCONV_INIT_C')\n"
    "assert(tonumber(a[0]) == 11)\n"
    "assert(tonumber(a[1]) == 13)\n"
    "assert(tonumber(a[2]) == 17)\n");
  seq6 = ljt_ctype_parse_seq(cts);
  assert(seq6 == seq5 + 2u);

  assert_cconv_init_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local p = ffi.new(lj_m7_cconv_init_pair_ct,\n"
    "  'LJ_M7_CCONV_INIT_A', 'LJ_M7_CCONV_INIT_B')\n"
    "assert(tonumber(p.a) == 11)\n"
    "assert(tonumber(p.b) == 13)\n");
  seq7 = ljt_ctype_parse_seq(cts);
  assert(seq7 == seq6 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local a = ffi.new(lj_m7_cconv_init_arr_ct, {\n"
    "  'LJ_M7_CCONV_INIT_A', 'LJ_M7_CCONV_INIT_B',\n"
    "  'LJ_M7_CCONV_INIT_C' })\n"
    "local p = ffi.new(lj_m7_cconv_init_pair_ct,\n"
    "  'LJ_M7_CCONV_INIT_A', 'LJ_M7_CCONV_INIT_B')\n"
    "local s = ffi.new(lj_m7_cconv_init_str_arr_ct, 'done')\n"
    "assert(tonumber(a[2]) == 17)\n"
    "assert(tonumber(p.b) == 13)\n"
    "assert(ffi.string(s) == 'done')\n");
  assert(ljt_ctype_parse_seq(cts) == seq7);

  lua_close(L);
  printf("t-ffi-cconv-init-snapshot OK: aggregate init waits on ctype snapshots\n");
  return 0;
}
