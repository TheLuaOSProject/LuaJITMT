/*
** Focused guard for ffi.typeinfo() ctype snapshots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"

#include "lib/lua_fixture_helpers.h"

static uint32_t parse_seq(CTState *cts)
{
  uint32_t seq = la_load32_acq(&cts->parse_token);
  assert((seq & 1u) == 0);
  return seq;
}

static uint32_t hold_parse_token(CTState *cts)
{
  uint32_t seq = parse_seq(cts);
  ctype_parse_token_rel(cts, seq + 1u);
  assert((ctype_parse_token_acq(cts) & 1u) != 0);
  return seq + 2u;
}

static void release_parse_token(CTState *cts, uint32_t seq)
{
  ctype_parse_token_rel(cts, seq);
  (void)ctype_parse_token_wake(cts, 1);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_typeinfo_snapshot_t;')\n"
    "lj_m7_typeinfo_snapshot_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_t'))\n"
    "assert(type(lj_m7_typeinfo_snapshot_id) == 'number')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "for i = 1, 100 do\n"
    "  local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_id)\n"
    "  assert(ti and ti.size == 4)\n"
    "end\n"
    "assert(ffi.typeinfo(0) == nil)\n"
    "assert(ffi.typeinfo(1000000000) == nil)\n");
  seq1 = parse_seq(cts);
  assert(seq1 == seq0);

  {
    uint32_t release_seq = hold_parse_token(cts);
    ljt_lua_dostring(L,
      "local ffi = require('ffi')\n"
      "assert(ffi.typeinfo(lj_m7_typeinfo_snapshot_id) == nil)\n");
    assert((ctype_parse_token_acq(cts) & 1u) != 0);
    release_parse_token(cts, release_seq);
  }

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_id)\n"
    "assert(ti and ti.size == 4)\n");
  assert(parse_seq(cts) == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef int lj_m7_typeinfo_snapshot_seq_t;')\n"
    "lj_m7_typeinfo_snapshot_seq_id = "
    "tonumber(ffi.typeof('lj_m7_typeinfo_snapshot_seq_t'))\n");
  seq2 = parse_seq(cts);
  assert(seq2 != seq1);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ti = ffi.typeinfo(lj_m7_typeinfo_snapshot_seq_id)\n"
    "assert(ti and ti.info ~= nil)\n");
  seq3 = parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-typeinfo-snapshot OK: stable typeinfo reads avoid parser locking\n");
  return 0;
}
