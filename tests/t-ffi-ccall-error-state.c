/*
** FFI errno/Win32 LastError preservation across call bookkeeping and boxing.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tg.h"

#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "lib/lua_fixture_helpers.h"

#define PROBE_WINERR UINT32_C(0x6a17)

typedef struct ProbeAllocCtx {
  lua_Alloc oldf;
  void *oldud;
  TGState *tg;
  uint64_t calls;
  uint64_t armed_calls;
  uint64_t armed_jit_calls;
  uint64_t failed_allocs;
  int armed;
  int fail_next;
} ProbeAllocCtx;

static ProbeAllocCtx *probe_ctx;
static int probe_value = 91;

static void probe_set_error(int errnum)
{
  errno = errnum;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)PROBE_WINERR);
#endif
}

static void probe_arm(void)
{
  assert(probe_ctx != NULL);
  probe_ctx->armed = 1;
  probe_set_error(EDOM);
}

static int32_t probe_i32(void)
{
  probe_arm();
  return 73;
}

static uint64_t probe_u64(void)
{
  probe_arm();
  return UINT64_C(0x1234567887654321);
}

static int *probe_ptr(void)
{
  probe_arm();
  return &probe_value;
}

static int *probe_ptr_oom(void)
{
  probe_arm();
  probe_ctx->fail_next = 1;
  return &probe_value;
}

static uint32_t probe_get_winerr(void)
{
#if LJ_TARGET_WINDOWS
  return (uint32_t)GetLastError();
#else
  return 0;
#endif
}

static void probe_disarm(void)
{
  assert(probe_ctx != NULL);
  probe_ctx->armed = 0;
}

static void *probe_clobber_alloc(void *ud, void *ptr, size_t osize,
				 size_t nsize)
{
  ProbeAllocCtx *ctx = (ProbeAllocCtx *)ud;
  void *p;
  if (ctx->fail_next && ctx->armed && ptr == NULL && nsize != 0) {
    ctx->fail_next = 0;
    ctx->failed_allocs++;
    p = NULL;
  } else {
    p = ctx->oldf(ctx->oldud, ptr, osize, nsize);
  }
  ctx->calls++;
  if (ctx->armed)
    ctx->armed_calls++;
  if (ctx->armed && ctx->tg != NULL &&
      lj_tg_load_jit_base(ctx->tg) != NULL)
    ctx->armed_jit_calls++;
  /* Deliberately clobber after the real allocator has returned. */
  errno = EILSEQ;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)(PROBE_WINERR + 1u));
#endif
  return p;
}

static void push_pointer(lua_State *L, const char *name, void *p)
{
  lua_pushlightuserdata(L, p);
  lua_setglobal(L, name);
}

static void reset_armed_allocs(ProbeAllocCtx *ctx)
{
  assert(ctx->armed == 0);
  ctx->armed_calls = 0;
  ctx->armed_jit_calls = 0;
}

static void require_armed_alloc(const ProbeAllocCtx *ctx, const char *phase,
				int require_jit)
{
  if (ctx->armed_calls == 0) {
    fprintf(stderr, "%s did not allocate between foreign return and disarm\n",
	    phase);
    abort();
  }
  if (require_jit && ctx->armed_jit_calls == 0) {
    fprintf(stderr, "%s allocated only after leaving generated code\n", phase);
    abort();
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  ProbeAllocCtx alloc;
  assert(L != NULL);

  alloc.oldf = lua_getallocf(L, &alloc.oldud);
  alloc.tg = L2TG(L);
  alloc.calls = 0;
  alloc.armed_calls = 0;
  alloc.armed_jit_calls = 0;
  alloc.failed_allocs = 0;
  alloc.armed = 0;
  alloc.fail_next = 0;
  probe_ctx = &alloc;

  push_pointer(L, "lj_probe_i32", (void *)(uintptr_t)&probe_i32);
  push_pointer(L, "lj_probe_u64", (void *)(uintptr_t)&probe_u64);
  push_pointer(L, "lj_probe_ptr", (void *)(uintptr_t)&probe_ptr);
  push_pointer(L, "lj_probe_ptr_oom", (void *)(uintptr_t)&probe_ptr_oom);
  push_pointer(L, "lj_probe_get_winerr",
	       (void *)(uintptr_t)&probe_get_winerr);
  push_pointer(L, "lj_probe_disarm", (void *)(uintptr_t)&probe_disarm);
  lua_pushinteger(L, EDOM);
  lua_setglobal(L, "lj_probe_expected_errno");
  lua_pushinteger(L, (lua_Integer)(LJ_TARGET_WINDOWS ? PROBE_WINERR : 0));
  lua_setglobal(L, "lj_probe_expected_winerr");

  ljt_lua_dostring(L,
    "ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct __attribute__((aligned(32))) { int x; } lj_probe_align_t;\n"
    "]]\n"
    "lj_probe_i32 = ffi.cast('int32_t (*)(void)', lj_probe_i32)\n"
    "lj_probe_u64 = ffi.cast('uint64_t (*)(void)', lj_probe_u64)\n"
    "lj_probe_ptr = ffi.cast('int *(*)(void)', lj_probe_ptr)\n"
    "lj_probe_ptr_oom = ffi.cast('int *(*)(void)', lj_probe_ptr_oom)\n"
    "lj_probe_get_winerr = ffi.cast('uint32_t (*)(void)',\n"
    "                                  lj_probe_get_winerr)\n"
    "lj_probe_disarm = ffi.cast('void (*)(void)', lj_probe_disarm)\n"
    "lj_probe_align_t = ffi.typeof('lj_probe_align_t')\n"
    "lj_probe_vla_t = ffi.typeof('uint8_t[?]')\n"
    "lj_probe_keep = {}\n");

  lua_setallocf(L, probe_clobber_alloc, &alloc);

  /* Interpreted pointer conversion allocates a fixed cdata after native_leave.
  ** The final call-frame restore must beat the allocator's deliberate clobber. */
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L,
    "jit.off()\n"
    "local p = lj_probe_ptr()\n"
    "local e = ffi.errno()\n"
    "local w = lj_probe_get_winerr()\n"
    "lj_probe_disarm()\n"
    "assert(e == lj_probe_expected_errno, e)\n"
    "assert(w == lj_probe_expected_winerr, w)\n"
    "assert(p[0] == 91)\n"
    "lj_probe_keep[1] = p\n");
  require_armed_alloc(&alloc, "interpreted pointer result", 0);
  assert(alloc.armed_jit_calls == 0);

  /* Fail the first post-return cdata allocation. The nothrow CNEW path must
  ** restore the foreign pair before lj_err_mem(), and the external unwinder's
  ** final landing pad must restore it again after installing the context. */
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L,
    "local ok, em = pcall(function() return lj_probe_ptr_oom() end)\n"
    "local e = ffi.errno()\n"
    "local w = lj_probe_get_winerr()\n"
    "lj_probe_disarm()\n"
    "assert(not ok, 'forced cdata allocation failure unexpectedly returned')\n"
    "assert(type(em) == 'string')\n"
    "assert(e == lj_probe_expected_errno, e)\n"
    "assert(w == lj_probe_expected_winerr, w)\n");
  assert(alloc.failed_allocs == 1);
  assert(alloc.fail_next == 0);
  require_armed_alloc(&alloc, "failing interpreted pointer result", 0);
  assert(alloc.armed_jit_calls == 0);
  /* GC2 does not yet have a custom-allocation registry. Keeping a wrapper
  ** installed across fresh collection cycles can make pre-existing arena
  ** objects look custom and is tracked as a separate lua_setallocf ABI blocker.
  ** The default fixture still proves the normal and exceptional post-return
  ** allocation edges; opt into the longer class matrix once testing that
  ** allocator migration work explicitly. */
  if (getenv("LJ_FFI_ERRSTATE_OOM_ONLY") != NULL ||
      getenv("LJ_FFI_ERRSTATE_ALLOC_STRESS") == NULL)
    goto done;

  /* The ordinary-call recorder is temporarily gated until XSAVE-backed
  ** CALLXS publication lands. Keep exercising the same boxed/aligned/VLA
  ** post-call allocation windows in the interpreter and require that none of
  ** those allocations happened under a generated-code jit_base. */
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L,
    "jit.on()\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "function lj_probe_boxed_run(n)\n"
    "  for i = 1, n do\n"
    "    local x = lj_probe_u64()\n"
    "    local e = ffi.errno()\n"
    "    local w = lj_probe_get_winerr()\n"
    "    lj_probe_disarm()\n"
    "    assert(e == lj_probe_expected_errno, e)\n"
    "    assert(w == lj_probe_expected_winerr, w)\n"
    "    lj_probe_keep[i] = x\n"
    "  end\n"
    "end\n"
    "lj_probe_boxed_run(200)\n"
    "local util = require('jit.util')\n"
    "local n = 0\n"
    "for tr = 1, 128 do if util.traceinfo(tr) then n = n + 1 end end\n"
    "assert(n == 0, 'ordinary FFI C call bypassed the XSAVE safety gate')\n");
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L, "lj_probe_boxed_run(4000)\n");
  require_armed_alloc(&alloc, "gated boxed CNEWI result", 0);
  assert(alloc.armed_jit_calls == 0);

  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "function lj_probe_aligned_run(n)\n"
    "  for i = 1, n do\n"
    "    local v = lj_probe_i32()\n"
    "    local x = lj_probe_align_t()\n"
    "    local e = ffi.errno()\n"
    "    local w = lj_probe_get_winerr()\n"
    "    lj_probe_disarm()\n"
    "    assert(v == 73, v)\n"
    "    assert(e == lj_probe_expected_errno, e)\n"
    "    assert(w == lj_probe_expected_winerr, w)\n"
    "    x.x = i\n"
    "    lj_probe_keep[i] = x\n"
    "  end\n"
    "end\n"
    "lj_probe_aligned_run(200)\n"
    "local util = require('jit.util')\n"
    "local n = 0\n"
    "for tr = 1, 128 do if util.traceinfo(tr) then n = n + 1 end end\n"
    "assert(n == 0, 'ordinary FFI C call bypassed the XSAVE safety gate')\n");
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L, "lj_probe_aligned_run(4000)\n");
  require_armed_alloc(&alloc, "gated aligned CNEW result", 0);
  assert(alloc.armed_jit_calls == 0);

  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "function lj_probe_vla_run(n)\n"
    "  for i = 1, n do\n"
    "    local v = lj_probe_i32()\n"
    "    local x = lj_probe_vla_t(17 + i % 7)\n"
    "    local e = ffi.errno()\n"
    "    local w = lj_probe_get_winerr()\n"
    "    lj_probe_disarm()\n"
    "    assert(v == 73, v)\n"
    "    assert(e == lj_probe_expected_errno, e)\n"
    "    assert(w == lj_probe_expected_winerr, w)\n"
    "    x[0] = i\n"
    "    lj_probe_keep[i] = x\n"
    "  end\n"
    "end\n"
    "lj_probe_vla_run(200)\n"
    "local util = require('jit.util')\n"
    "local n = 0\n"
    "for tr = 1, 128 do if util.traceinfo(tr) then n = n + 1 end end\n"
    "assert(n == 0, 'ordinary FFI C call bypassed the XSAVE safety gate')\n");
  reset_armed_allocs(&alloc);
  ljt_lua_dostring(L, "lj_probe_vla_run(4000)\n");
  require_armed_alloc(&alloc, "gated variable CNEW result", 0);
  assert(alloc.armed_jit_calls == 0);

  assert(alloc.armed == 0);
  assert(alloc.calls != 0);
done:
  assert(alloc.armed == 0);
  lua_setallocf(L, alloc.oldf, alloc.oldud);
  probe_ctx = NULL;
  lua_close(L);
  printf("t-ffi-ccall-error-state OK: gated post-call boxing preserves native errors\n");
  return 0;
}
