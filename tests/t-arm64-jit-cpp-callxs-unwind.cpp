/* Generated ARM64 CALLXS -> C++ -> Lua callback unwind integration proof. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_ccall.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_trace.h"
#include "lib/lua_fixture_helpers.h"
}

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

typedef int (*LuaCallback)(int);
typedef int (*NativeObserver)(int);
typedef void (*InstallObserver)(NativeObserver);
typedef void (*InstallCallback)(LuaCallback);
typedef unsigned (*ReadCounter)(void);

static TGState *test_tg;
static void *bridge_address;
static unsigned generated_calls;
static unsigned generated_error_calls;
static TraceNo error_trace;

/* This is a plain native function pointer installed with dlsym, not a Lua
   callback. Inspect the frame before the C++ bridge invokes its Lua callback. */
static int observe_native_call(int callback_armed)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result =
    lj_ffi_native_frame_snapshot(test_tg, &snapshot);
  const LJFFINativeFrame *frame;
  GCtrace *trace;
  uint32_t flags;
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY) return 0;
  assert(result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1);
  frame = &snapshot.frame[0];
  flags = lj_ffi_native_frame_flags_acq(frame);
  assert((flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
                  LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
                  LJ_FFI_NATIVE_FRAME_F_POSTCALL)) ==
         LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  assert((flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0);
  assert(lj_ffi_native_frame_func_acq(frame) == bridge_address);
  assert(lj_ffi_native_frame_L_acq(frame) == lj_tg_load_cur_L(test_tg));
  assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
  trace = lj_ffi_native_frame_trace_acq(frame);
  assert(trace != NULL && trace_native_pins_acq(trace) != 0);
  assert(lj_ffi_native_frame_trace_no_acq(frame) != 0);
  assert(lj_tg_in_native_acq(test_tg) == 1);
  assert(lj_tg_load_jit_base(test_tg) != NULL);
  assert(lj_tg_vmstate_load_acq(test_tg) ==
         (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  generated_calls++;
  if (callback_armed) {
    generated_error_calls++;
    error_trace = lj_ffi_native_frame_trace_no_acq(frame);
  }
  return 1;
}

static void assert_native_state_clean(void)
{
  unsigned i;
  assert((lj_ffi_native_frame_sequence_acq(test_tg) & 1u) == 0);
  assert(lj_ffi_native_frame_depth_acq(test_tg) == 0);
  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_tg_load_jit_base(test_tg) == NULL);
  assert(test_tg->ffi_xsave_root == NULL);
  assert(test_tg->ffi_xsave_baseslot == 0);
  assert(test_tg->ffi_xsave_nslots == 0);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    const LJFFINativeFrame *frame = &test_tg->ffi_native_frame[i];
    assert(lj_ffi_native_frame_trace_acq(frame) == NULL);
    assert(lj_ffi_native_frame_L_acq(frame) == NULL);
    assert(lj_ffi_native_frame_func_acq(frame) == NULL);
    assert(lj_ffi_native_frame_result_root_acq(frame) == NULL);
    assert(lj_ffi_native_frame_flags_acq(frame) == 0);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  const char *path = getenv("LJ_ARM64_CPP_CALLXS_LIB");
  int stack_args = getenv("LJ_ARM64_CPP_CALLXS_STACK") != NULL;
  void *library;
  InstallObserver install_observer;
  InstallCallback install_callback;
  ReadCounter calls, unwinds, destructors;
  unsigned warm_generated, before_error, before_recovery, recovery_generated;
  assert(L != NULL && path != NULL);
  test_tg = L2TG(L);
  library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  assert(library != NULL);
  bridge_address = dlsym(library, stack_args ? "arm64_cpp_stack_bridge" :
                        "arm64_cpp_call_bridge");
  install_observer = (InstallObserver)dlsym(library, "arm64_cpp_install_observer");
  install_callback = (InstallCallback)dlsym(library, "arm64_cpp_install_callback");
  calls = (ReadCounter)dlsym(library, "arm64_cpp_bridge_calls");
  unwinds = (ReadCounter)dlsym(library, "arm64_cpp_unwinds");
  destructors = (ReadCounter)dlsym(library, "arm64_cpp_destructors");
  assert(bridge_address && install_observer && install_callback &&
         calls && unwinds && destructors);
  install_observer(observe_native_call);
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef int (*arm64_lua_cb)(int); "
    "int arm64_cpp_call_bridge(int); "
    "int arm64_cpp_stack_bridge(int,int,int,int,int,int,int,int,int,int,int,int,int); "
    "void arm64_cpp_install_callback(arm64_lua_cb);]]\n"
    "local lib = ffi.load(assert(os.getenv('LJ_ARM64_CPP_CALLXS_LIB')))\n"
    "local bridge = lib.arm64_cpp_call_bridge\n"
    "local function run_bridge(fn, n)\n"
    "  for i = 1, n do if fn(i) ~= i then return false end end\n"
    "  return true\n"
    "end\n"
    "if os.getenv('LJ_ARM64_CPP_CALLXS_STACK') then\n"
    "  bridge = lib.arm64_cpp_stack_bridge\n"
    "  run_bridge = function(fn, n)\n"
    "    for i=1,n do\n"
    "      if fn(101,202,303,404,505,606,707,808,909,1010,1111,1212,i) ~= i then return false end\n"
    "    end\n"
    "    return true\n"
    "  end\n"
    "end\n"
    "_G.__arm64_cpp_run = run_bridge\n"
    "_G.__arm64_cpp_bridge = bridge\n"
    "_G.__arm64_cpp_lib = lib\n"
    "jit.flush(); jit.on(); jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "assert(run_bridge(bridge, 400))\n");
  assert(generated_calls != 0 && generated_error_calls == 0);
  assert(calls() == 400 && destructors() == calls() && unwinds() == 0);
  assert_native_state_clean();
  warm_generated = generated_calls;
  before_error = calls();
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "__arm64_cpp_bad = ffi.cast('arm64_lua_cb', function() "
    "error('cpp-callxs-unwind-marker') end)\n"
    "__arm64_cpp_lib.arm64_cpp_install_callback(__arm64_cpp_bad)\n"
    "local ok, err = pcall(__arm64_cpp_run, __arm64_cpp_bridge, 400)\n"
    "assert(not ok and string.find(tostring(err), "
    "'cpp-callxs-unwind-marker', 1, true))\n");
  assert(generated_error_calls == 1 && error_trace != 0);
  assert(generated_calls > warm_generated && calls() > before_error);
  assert(destructors() == calls() && unwinds() == 1);
  assert_native_state_clean();
  {
    GCtrace *trace = traceref_safe(G2J(G(L)), error_trace);
    assert(trace != NULL && trace_native_pins_acq(trace) == 0);
  }
  install_callback(NULL);
  before_recovery = calls();
  recovery_generated = generated_calls;
  ljt_lua_dostring(L,
    "__arm64_cpp_bad:free(); __arm64_cpp_bad = nil\n"
    "collectgarbage('collect')\n"
    "assert(__arm64_cpp_run(__arm64_cpp_bridge, 400))\n"
    "jit.flush(); collectgarbage('collect')\n"
    "assert(__arm64_cpp_run(__arm64_cpp_bridge, 400))\n");
  assert(calls() == before_recovery + 800);
  assert(destructors() == calls() && unwinds() == 1);
  assert(generated_error_calls == 1);
  assert(generated_calls > recovery_generated);
  assert_native_state_clean();
  install_observer(NULL);
  lua_close(L);
  assert(dlclose(library) == 0);
  printf("t-arm64-jit-cpp-callxs-unwind OK: %s, %u generated calls; "
         "trace %u unwound through C++/Lua callback, cleaned roots, "
         "and recovered after GC/flush\n", stack_args ? "stack args" : "register args",
         generated_calls, (unsigned)error_trace);
  return 0;
}

#else
int main(void)
{
  puts("t-arm64-jit-cpp-callxs-unwind SKIP: requires experimental Darwin ARM64");
  return 0;
}
#endif
