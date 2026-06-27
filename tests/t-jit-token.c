/*
** Focused guard for the M6 JIT recorder token.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_target.h"

#include "lib/lua_fixture_helpers.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;

  g = G(L);
  assert(jit_token_acq(g) == 0);

  {
    GCtrace trace;
    SnapShot snap;
    uint8_t count;
    memset(&trace, 0, sizeof(trace));
    memset(&snap, 0, sizeof(snap));

    trace_nchild_inc_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 1);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 1) != 0);
    assert(count == 0);
    assert(snap_count_acq(&snap) == 1);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 2) == 0);
    assert(count == 1);
    assert(snap_count_acq(&snap) == 1);

    snap_count_rel(&snap, SNAPCOUNT_DONE);
    assert(snap_count_acq(&snap) == SNAPCOUNT_DONE);
  }

#if LJ_TARGET_X64 && !LJ_ABI_WIN
  {
    TGState secondary;
    TGState *saved_tg = lj_thr_get_tg();
    assert(g->main_tg != NULL);
    lj_tg_init_thread(g, &secondary, NULL, 0);
    secondary.tid = g->main_tg->tid == 0x7ffffffeu ? 0x7ffffffdu : 0x7ffffffeu;
    secondary.alloc.owner_tid = secondary.tid;
    lj_thr_set_tg(&secondary);
    assert(G2TG(g) == &secondary);
    assert(lj_jit_token_try(g->jitp) != 0);
    assert(jit_token_acq(g) == secondary.tid);
    assert(lj_jit_token_held(g->jitp) != 0);
    lj_jit_token_release(g->jitp);
    assert(jit_token_acq(g) == 0);
    lj_thr_set_tg(saved_tg);
    lj_tg_fini_thread(g, &secondary);
  }
#endif

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'expected token-owned recording')\n");
  assert(jit_token_acq(g) == 0);

  ljt_lua_dostring(L, "jit.flush()");
  jit_token_rel(g, 0x7fffffffu);
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do assert(f(80) == 3240) end\n"
    "assert(tracecount() == 0, 'busy recorder token must skip tracing')\n");
  assert(jit_token_acq(g) == 0x7fffffffu);

  jit_token_rel(g, 0);
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'recording should resume after token release')\n");
  assert(jit_token_acq(g) == 0);

  lua_close(L);
  printf("t-jit-token OK: recorder token accepts secondary TGs and skips busy recording\n");
  return 0;
}
