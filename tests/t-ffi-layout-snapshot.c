/*
** Focused guard for lock-free ffi.sizeof/alignof/offsetof layout snapshots.
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
#include "lj_cdata.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
  int saw_native;
} ParseReleaseCtx;

static void init_abandoned_ctype(CType *ct)
{
  ctype_info_rel(ct, CTINFO(CT_ATTRIB, CTATTRIB(CTA_BAD)));
  ctype_size_rel(ct, 0);
  ctype_sib_rel(ct, 0);
  ctype_next_rel(ct, 0);
  ctype_clearname(ct);
}

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

static void assert_layout_waits_without_lock(lua_State *L, CTState *cts,
					     TGState *tg, const char *chunk)
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

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3, seq4, seq5;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef([[\n"
    "typedef struct { int a; double b; } lj_m7_layout_snapshot_t;\n"
    "typedef struct { unsigned int a; unsigned int b:5; } lj_m7_layout_bits_t;\n"
    "]])\n"
    "lj_m7_ffi = ffi\n"
    "lj_m7_layout_snapshot_ct = ffi.typeof('lj_m7_layout_snapshot_t')\n"
    "lj_m7_layout_bits_ct = ffi.typeof('lj_m7_layout_bits_t')\n"
    "lj_m7_layout_int_ct = ffi.typeof('int')\n"
    "lj_m7_layout_vla_ct = ffi.typeof('int [?]')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  {
    CType *ct;
    CTypeID id = lj_ctype_new_l(L, cts, &ct);
    GCcdata *cd;
    init_abandoned_ctype(ct);
    cd = lj_cdata_new_(L, CTID_CTYPEID, sizeof(CTypeID));
    *(CTypeID *)cdataptr(cd) = id;
    setcdataV(L, L->top++, cd);
    lua_setglobal(L, "lj_m7_layout_abandoned_ct");
  }

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "for i = 1, 100 do\n"
    "  assert(ffi.sizeof(lj_m7_layout_snapshot_ct) == 16)\n"
    "  assert(ffi.alignof(lj_m7_layout_snapshot_ct) == 8)\n"
    "  assert(ffi.offsetof(lj_m7_layout_snapshot_ct, 'a') == 0)\n"
    "  assert(ffi.offsetof(lj_m7_layout_snapshot_ct, 'b') == 8)\n"
    "  local ofs, bitpos, bitsz = ffi.offsetof(lj_m7_layout_bits_ct, 'b')\n"
    "  assert(ofs == 4 and bitpos == 0 and bitsz == 5)\n"
    "  assert(ffi.sizeof(lj_m7_layout_vla_ct, 7) == 28)\n"
    "  local obj = ffi.new(lj_m7_layout_snapshot_ct, { a = i, b = i + 0.5 })\n"
    "  assert(obj.a == i and obj.b == i + 0.5)\n"
    "  local arr = ffi.new(lj_m7_layout_vla_ct, 7)\n"
    "  arr[6] = i\n"
    "  assert(ffi.sizeof(arr) == 28 and arr[6] == i)\n"
    "end\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof(lj_m7_layout_abandoned_ct) == nil)\n"
    "assert(ffi.alignof(lj_m7_layout_abandoned_ct) == nil)\n"
    "local ok, err = pcall(ffi.new, lj_m7_layout_abandoned_ct)\n"
    "assert(ok == false and tostring(err):match('size'))\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof(lj_m7_layout_snapshot_ct) == 16)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.alignof(lj_m7_layout_snapshot_ct) == 8)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.offsetof(lj_m7_layout_snapshot_ct, 'b') == 8)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof(lj_m7_layout_vla_ct, 7) == 28)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 8u);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local obj = ffi.new(lj_m7_layout_snapshot_ct)\n"
    "obj.a = 17\n"
    "assert(obj.a == 17)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 10u);

  assert_layout_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "local v = ffi.cast(lj_m7_layout_int_ct, 23)\n"
    "assert(tonumber(v) == 23)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 12u);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local ffi = lj_m7_ffi\n"
      "local ct = lj_m7_layout_snapshot_ct\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
      "jit.flush()\n"
      "jit.on()\n"
      "jit.opt.start('hotloop=1', 'hotexit=1')\n"
      "local function run(n)\n"
      "  local sum = 0\n"
      "  for i = 1, n do\n"
      "    local obj = ffi.new(ct)\n"
      "    obj.a = i\n"
      "    obj.b = i + 0.5\n"
      "    sum = sum + obj.a\n"
      "  end\n"
      "  return sum\n"
      "end\n"
      "for i = 1, 3 do assert(run(8) == 36) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = lj_m7_ffi\n"
    "local ct = lj_m7_layout_snapshot_ct\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    local obj = ffi.new(ct)\n"
    "    obj.a = i\n"
    "    obj.b = i + 0.5\n"
    "    sum = sum + obj.a\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 820) end\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.offsetof('struct { int a; double b; }', 'b') == 8)\n");
  seq4 = ljt_ctype_parse_seq(cts);
  assert(seq4 == seq3 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.sizeof('struct { int lock_path; }') == 4)\n");
  seq5 = ljt_ctype_parse_seq(cts);
  assert(seq5 != seq4);

  lua_close(L);
  printf("t-ffi-layout-snapshot OK: stable layout queries avoid cparser sequence\n");
  return 0;
}
