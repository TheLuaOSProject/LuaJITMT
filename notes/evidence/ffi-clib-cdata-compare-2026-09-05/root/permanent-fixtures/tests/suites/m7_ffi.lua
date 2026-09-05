local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")

local getenv = utils.getenv
local lua_path = runtime.lua_path
local build_and_run_c = build.compile_and_run_c
local run_c_fixture_specs = build.run_c_fixture_specs
local build_shared_library = build.build_shared_library
local write_ld_script = build.write_ld_script
local clean_build = build.clean_build
local luajit_code = runtime.luajit_code
local luajit_file = runtime.luajit_file
local run_luajit_script = runtime.luajit_script
local run_stock = runtime.run_stock

local function build_and_run_cpp(t, out, cppfile, opts)
  local old_compiler = t.compiler
  local ok, err
  t.compiler = getenv("CXX", "c++")
  ok, err = xpcall(function()
    build_and_run_c(t, out, cppfile, opts)
  end, debug.traceback)
  t.compiler = old_compiler
  if not ok then error(err, 0) end
end

local function read_all(path)
  local f = assert(io.open(path, "rb"))
  local s = assert(f:read("*a"))
  f:close()
  return s
end

local function plain_count(s, needle)
  local n, pos = 0, 1
  while true do
    local first = s:find(needle, pos, true)
    if not first then return n end
    n = n + 1
    pos = first + #needle
  end
end

local function assert_generic_ccall_source(t)
  local files = {
    t:path("src", "lj_crecord.c"),
    t:path("src", "lj_ccall.c"),
    t:path("src", "lj_ccall.h"),
    t:path("src", "lj_ircall.h")
  }
  for _, path in ipairs(files) do
    local source = read_all(path)
    assert(plain_count(source, "crec_call_jit_") == 0,
           path .. ": explicit recorder shape survived")
    assert(plain_count(source, "lj_ccall_jit_") == 0,
           path .. ": explicit C-call wrapper survived")
    assert(plain_count(source, "LJ_CCALL_JIT_") == 0,
           path .. ": explicit signature enum survived")
  end
  local recorder = read_all(files[1])
  assert(plain_count(recorder, "LJ_FFI_CALLXS_TEST_ACTIVATE") == 0,
         "generic CALLXS still has a test-only production gate")
  assert(plain_count(recorder, "static IRType crec_ct2irt(") == 0,
         "recorder retained a non-snapshot CType-to-IR conversion")
  assert(plain_count(recorder,
         "static MSize crec_call_args_collect(") == 1,
         "generic C-call argument collector is not unique")
  assert(plain_count(recorder,
         "static TRef crec_call_args_emit(") == 1,
         "generic C-call argument emitter is not unique")
  assert(plain_count(recorder, "IRT(IR_CALLXS, t)") == 1,
         "generic CALLXS emission is not unique")
end

local m7_cases = {
  "m7_ffi_cdef_token",
  "m7_ffi_cdef_dup_stack",
  "m7_ffi_cparse_rollback",
  "m7_ffi_typeinfo_snapshot",
  "m7_ffi_ctype_intern_l",
  "m7_ffi_ctype_hash_publish",
  "m7_ffi_ctype_tab_retire",
  "m7_ffi_ctype_ticket_intern",
  "m7_ffi_ctype_name_claim",
  "m7_ffi_ctype_pointer_ids",
  "m7_ffi_cdata_alloc",
  "m7_ffi_jit_cnew",
  "m7_ffi_snap_restore_l",
  "m7_ffi_finreg",
  "m7_ffi_metatype",
  "m7_ffi_cdata_get_l",
  "m7_ffi_cdata_set_l",
  "m7_ffi_cdata_shared_hammer",
  "m7_ffi_carith_l",
  "m7_ffi_clib_cache",
  "m7_ffi_clib_receiver",
  "m7_ffi_clib_cache_authority",
  "m7_ffi_clib_cdata_compare",
  "m7_ffi_clib_ldscript",
  "m7_ffi_nested_state",
  "m7_ffi_callback_install",
  "m7_ffi_callback_runtime",
  "m7_ffi_callxs_authentic",
  "m7_ffi_callxs_callback_stack",
  "m7_ffi_callxs_sysv_small_aggregate",
  "m7_ffi_ccall_native",
  "m7_ffi_native_frames"
}

local function build_clib_ldscript_fixture(t)
  local script = t:tmp("lj_t-ffi-clib-ldscript.so")
  local so = build_shared_library(t, t:tmp("lj_t-ffi-clib-ldscript-real.so"),
                                  "t-ffi-clib-ldscript-lib.c")
  return write_ld_script(script, so)
end

return function(add)
  add({
    name = "m7_ffi_callxs_callback_stack",
    description = "generated callbacks preserve XSAVE stack extent and unwind",
    run = function(t)
      build.build_default(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-callxs-callback-stack"),
                      "t-ffi-callxs-callback-stack.c", {
        build = false,
        timeout = "30s"
      })
      print("M7 generated callback stack geometry passed")
    end
  })

  add({
    name = "m7_ffi_callxs_authentic",
    description = "production generic CALLXS scalar/boxed/bool/sret lifecycle",
    run = function(t)
      local flags = table.concat({
        "-DLUA_USE_ASSERT",
        "-DLJ_XSAVE_TEST_HELPERS"
      }, " ")
      local callxs_so = build_shared_library(t,
        t:tmp("lj_t-ffi-callxs-authentic.so"),
        "t-ffi-callxs-authentic-lib.c")
      local callxs_flush_so = build_shared_library(t,
        t:tmp("lj_t-ffi-callxs-remote-flush.so"),
        "t-ffi-callxs-remote-flush-lib.c")
      build.with_default_build_restore(t, function()
        clean_build(t, { quiet = true, xcflags = flags })
        run_luajit_script(t, "t-ffi-callxs-authentic.lua", nil, {
          env = { LJ_M7_FFI_CALLXS_SO = callxs_so },
          timeout = "30s"
        })
        run_luajit_script(t, "t-ffi-callxs-remote-flush.lua", nil, {
          env = { LJ_M7_FFI_CALLXS_FLUSH_SO = callxs_flush_so },
          timeout = "30s"
        })
        build_and_run_c(t, t:tmp("lj_t-ffi-callxs-postcall"),
                        "t-ffi-callxs-postcall.c", {
          build = false,
          cflags = flags,
          env = { LJ_M7_FFI_CALLXS_SO = callxs_so },
          timeout = "30s"
        })
        build_and_run_c(t, t:tmp("lj_t-ffi-callxs-callback"),
                        "t-ffi-callxs-callback.c", {
          build = false,
          cflags = flags,
          timeout = "30s"
        })
      end)
      print("M7 production generic CALLXS lifecycle passed")
    end
  })

  add({
    name = "m7_ffi_callxs_sysv_small_aggregate",
    description = "generic SysV x64 one-class aggregate CALLXS ABI",
    run = function(t)
      clean_build(t)
      local aggregate_so = build_shared_library(t,
        t:tmp("lj_t-ffi-callxs-sysv-small-aggregate.so"),
        "t-ffi-callxs-sysv-small-aggregate-lib.c")
      run_luajit_script(t, "t-ffi-callxs-sysv-small-aggregate.lua", nil, {
        env = { LJ_M7_FFI_CALLXS_SYSV_AGG_SO = aggregate_so },
        timeout = "30s"
      })
      print("M7 generic SysV small-aggregate CALLXS ABI passed")
    end
  })

  add({
    name = "m7_ffi_ccall_native",
    description = "FFI native state and production generic CALLXS",
    run = function(t)
      local root_so, struct_so, jit_so
      assert_generic_ccall_source(t)
      clean_build(t)
      jit_so = build_shared_library(t,
        t:tmp("lj_t-ffi-ccall-jit-lib.so"),
        "t-ffi-ccall-jit-lib.c")
      struct_so = build_shared_library(t,
        t:tmp("lj_t-ffi-ccall-struct-overflow.so"),
        "t-ffi-ccall-struct-overflow-lib.c")
      root_so = build_shared_library(t,
        t:tmp("lj_t-ffi-ccall-temp-roots.so"),
        "t-ffi-ccall-temp-roots-lib.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-cbblack-race"),
                      "t-ffi-cbblack-race.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-ccall-native-helpers"),
                      "t-ffi-ccall-native-helpers.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-ccall-error-state"),
                      "t-ffi-ccall-error-state.c", {
                        env = { LJ_FFI_ERRSTATE_ALLOC_STRESS = "1" },
                        timeout = "30s"
                      })
      -- The focused production fixture mechanically requires XSAVE/CALLXS.
      -- The larger ABI catalogue below proves the same generic recorder across
      -- the complete admitted scalar and boxed result matrix.
      run_luajit_script(t, "t-ffi-callxs-production.lua")
      run_luajit_script(t, "t-ffi-ccall-temp-roots.lua", nil, {
        env = { LJ_M7_FFI_CCALL_ROOT_SO = root_so },
        timeout = "60s"
      })
      build_and_run_c(t, t:tmp("lj_t-ffi-ccall-struct-overflow"),
                      "t-ffi-ccall-struct-overflow.c", {
        env = { LJ_M7_FFI_CCALL_STRUCT_SO = struct_so },
        timeout = "20s"
      })
      build_and_run_c(t, t:tmp("lj_t-ffi-lib-native-stopreq"),
                      "t-ffi-lib-native-stopreq.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-ccall-stopreq"),
                      "t-ffi-ccall-stopreq.c", {
        env = { LJ_M7_FFI_CCALL_JIT_SO = jit_so },
        timeout = "20s"
      })
      -- Keep every legacy ABI/result assertion live. No production wrapper or
      -- signature dispatcher backs these calls; all admitted scalar and boxed
      -- rows must execute the one generic CALLXS lifecycle.
      run_luajit_script(t, "t-ffi-ccall-native.lua", nil, {
        env = {
          LJ_M7_FFI_CCALL_JIT_SO = jit_so,
          LJ_M7_FFI_CCALL_MIXED_RESULTS = "1"
        },
        timeout = "60s"
      })
      luajit_code(t, [[
local ffi = require"ffi"
local util = require"jit.util"
local dst = ffi.new("uint8_t[512]")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
for _ = 1, 80 do ffi.fill(dst, 256, 0x5a) end
assert(dst[0] == 0x5a and dst[255] == 0x5a)
assert(util.traceinfo(1), "bulk ffi.fill loop did not trace")
print("bulk fill ok")
]], { timeout = "60s" })
      print("M7 FFI native blocking-call behavior passed")
    end
  })

  add({
    name = "m7_ffi_native_frames",
    description = "generic FFI native-frame sequence and nesting substrate",
    run = function(t)
      local flags = "-DLUA_USE_ASSERT -DLJ_FFI_NATIVE_FRAME_TEST_HELPERS"
      build.with_default_build_restore(t, function()
        clean_build(t, { quiet = true, xcflags = flags })
        build_and_run_c(t, t:tmp("lj_t-ffi-native-frames"),
                        "t-ffi-native-frames.c", {
          build = false,
          cflags = flags,
          timeout = "20s"
        })
      end)
      print("M7 FFI native-frame substrate passed")
    end
  })

  add({
    name = "m7_ffi_callback_install",
    description = "FFI callback slot install behavior",
    run = function(t)
      local pthread = getenv("PTHREAD", "-pthread")
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-mcode-native"),
                      "t-ffi-callback-mcode-native.c", {
        libs = { "-lm", "-ldl", pthread,
                 "-Wl,--wrap=mmap", "-Wl,--wrap=mmap64",
                 "-Wl,--wrap=mprotect" },
        timeout = "20s"
      })
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-snapshot"),
                      "t-ffi-callback-snapshot.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-callback-install.lua", {
        getenv("LJ_M7_FFI_CBACK_THREADS", "6"),
        getenv("LJ_M7_FFI_CBACK_ITERS", "64")
      }, { joff = true })
      print("M7 FFI callback slot install behavior passed")
    end
  })

  add({
    name = "m7_ffi_callback_runtime",
    description = "FFI callback runtime behavior",
    run = function(t)
      clean_build(t)
      run_c_fixture_specs(t, {
        { output = "lj_t-ffi-callback-nested-native",
          cfile = "t-ffi-callback-nested-native.c" },
        { output = "lj_t-ffi-callback-owner-lifetime",
          cfile = "t-ffi-callback-owner-lifetime.c" },
        { output = "lj_t-ffi-callback-stopreq",
          cfile = "t-ffi-callback-stopreq.c" },
        { output = "lj_t-ffi-callback-attached-carrier",
          cfile = "t-ffi-callback-attached-carrier.c" },
        { output = "lj_t-ffi-callback-auto-attach",
          cfile = "t-ffi-callback-auto-attach.c",
          opts = { timeout = "20s" } }
      })
      build_and_run_cpp(t, t:tmp("lj_t-ffi-callback-auto-unwind"),
                        "t-ffi-callback-auto-unwind.cpp", {
        build = false,
        default_cflags = false,
        cflags = table.concat({
          "-std=gnu++11", "-O2", "-Wall", "-Wextra", "-Werror",
          "-mcx16", "-fexceptions", "-funwind-tables"
        }, " "),
        timeout = "20s"
      })
      run_luajit_script(t, "t-ffi-callback-runtime.lua", {
        getenv("LJ_M7_FFI_CBACK_RT_THREADS", "6"),
        getenv("LJ_M7_FFI_CBACK_RT_ITERS", "220")
      }, { joff = true })
      luajit_file(t, t:path("tests", "stock", "test", "lib", "ffi",
                            "ffi_callback.lua"))
      print("M7 FFI callback runtime behavior passed")
    end
  })

  add({
    name = "m7_ffi_carith_l",
    description = "FFI arithmetic/raw conversion behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-carith-check64-snapshot"),
                      "t-ffi-carith-check64-snapshot.c",
                      { build = false, timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-carith-arg-snapshot"),
                      "t-ffi-carith-arg-snapshot.c",
                      { build = false, timeout = "20s" })
      run_luajit_script(t, "t-ffi-carith-l.lua", nil, { joff = true })
      print("M7 FFI arithmetic/raw conversion behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdata_alloc",
    description = "concurrent FFI cdata allocation behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-cdata-pre-ctstate"),
                      "t-ffi-cdata-pre-ctstate.c",
                      { build = false, timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-gc2-overaligned-cdata-progress"),
                      "t-gc2-overaligned-cdata-progress.c",
                      { build = false, timeout = "20s" })
      run_luajit_script(t, "t-ffi-cdata-alloc.lua", {
        getenv("LJ_M7_FFI_CDATA_THREADS", "6"),
        getenv("LJ_M7_FFI_CDATA_ITERS", "400")
      }, { joff = true })
      print("M7 FFI cdata allocation behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdata_get_l",
    description = "FFI cdata read behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdata-get-l.lua", {
        getenv("LJ_M7_FFI_GET_THREADS", "6"),
        getenv("LJ_M7_FFI_GET_ITERS", "400")
      }, { joff = true })
      print("M7 FFI cdata read behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdata_set_l",
    description = "FFI write behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true, timeout = "20s" })
      print("M7 FFI cdata write behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdata_shared_hammer",
    description = "shared FFI cdata field hammer behavior",
    run = function(t)
      clean_build(t)
      local args = {
        getenv("LJ_M7_FFI_SHARED_THREADS", "6"),
        getenv("LJ_M7_FFI_SHARED_ITERS", "24000")
      }
      run_luajit_script(t, "t-ffi-cdata-shared-hammer.lua", args,
			{ joff = true, timeout = "30s" })
      run_luajit_script(t, "t-ffi-cdata-shared-hammer.lua", args,
			{ timeout = "30s" })
      print("M7 shared FFI cdata field hammer behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdef_dup_stack",
    description = "duplicate ffi.cdef/string-ctype stack-growth race behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-dup-stack.lua", {
        getenv("LJ_M7_FFI_DUP_STACK_ROUNDS", "30"),
        getenv("LJ_M7_FFI_DUP_STACK_ITERS", "200")
      }, { joff = true, timeout = "30s" })
      print("M7 FFI duplicate cdef stack-growth behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdef_token",
    description = "parser-driven FFI CTState mutation behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-cdef-token-stopreq"),
                      "t-ffi-cdef-token-stopreq.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      print("M7 FFI cdef token behavior passed")
    end
  })

  add({
    name = "m7_ffi_clib_cache",
    description = "FFI C library cache miss/fill behavior",
    run = function(t)
      build.with_default_build_restore(t, function()
        local helper_flag = "-DLJ_CLIB_TEST_HELPERS"
        clean_build(t, { quiet = true, xcflags = helper_flag })
        local extern_so = build_shared_library(t,
          t:tmp("lj_t-ffi-clib-extern-snapshot.so"),
          "t-ffi-clib-extern-snapshot-lib.c")
        build_and_run_c(t, t:tmp("lj_t-ffi-clib-extern-snapshot"),
                        "t-ffi-clib-extern-snapshot.c", {
          build = false,
          env = { LJ_M7_FFI_CLIB_EXTERN_SO = extern_so },
          timeout = "20s"
        })
        build_and_run_c(t, t:tmp("lj_t-ffi-clib-cache-retire"),
                        "t-ffi-clib-cache-retire.c", {
          build = false,
          timeout = "20s"
        })
        build_and_run_c(t, t:tmp("lj_t-ffi-clib-deferred-close"),
                        "t-ffi-clib-deferred-close.c", {
          build = false,
          cflags = helper_flag,
          env = { LJ_M7_FFI_CLIB_CLOSE_SO = extern_so },
          timeout = "20s"
        })
        run_luajit_script(t, "t-ffi-clib-close-race.lua", nil, {
          env = { LJ_M7_FFI_CLIB_CLOSE_SO = extern_so },
          joff = true,
          timeout = "20s"
        })
        run_luajit_script(t, "t-ffi-clib-cache.lua", {
          getenv("LJ_M7_FFI_CLIB_THREADS", "6"),
          getenv("LJ_M7_FFI_CLIB_ITERS", "300")
        }, { joff = true })
        run_luajit_script(t, "t-ffi-clib-cache.lua", {
          getenv("LJ_M7_FFI_CLIB_JIT_THREADS", "2"),
          getenv("LJ_M7_FFI_CLIB_JIT_ITERS", "180")
        })
      end)
      print("M7 FFI clib cache behavior passed")
    end
  })

  add({
    name = "m7_ffi_clib_receiver",
    description = "captured namespace methods retain and guard their receiver",
    run = function(t)
      if jit.os ~= "Linux" or jit.arch ~= "x64" then
        print("M7 captured namespace receiver fixtures require Linux/x64")
        return
      end
      build.build_default(t)
      local libraries = {}
      for _, value in ipairs({ 11, 29 }) do
        libraries[#libraries + 1] = build_shared_library(t,
          t:tmp("lj_t-ffi-clib-receiver-" .. value .. ".so"),
          "t-ffi-clib-receiver-lib.c", {
            cflags = "-O2 -Wall -Wextra -Werror -DNAMESPACE_VALUE=" .. value
          })
      end
      for _, mode in ipairs({
        "index-other", "index-type", "newindex-other", "newindex-type",
        "index-life", "newindex-life", "index-side-other", "index-side-type",
        "newindex-side-other", "newindex-side-type"
      }) do
        local args = { mode, libraries[1], libraries[2] }
        run_luajit_script(t, "t-ffi-clib-receiver.lua", args, {
          joff = true, timeout = "20s"
        })
        run_luajit_script(t, "t-ffi-clib-receiver.lua", args, {
          jon = true, timeout = "20s"
        })
      end
      print("M7 captured namespace receiver guards passed")
    end
  })

  add({
    name = "m7_ffi_clib_cache_authority",
    description = "native namespace cache writes, close, GC and recorder cleanup",
    run = function(t)
      if jit.os ~= "Linux" or jit.arch ~= "x64" then
        print("M7 namespace cache authority fixtures require Linux/x64 and GNU ld wrapping")
        return
      end
      build.build_default(t)
      local library = build_shared_library(t,
        t:tmp("lj_t-ffi-clib-cache-authority.so"),
        "t-ffi-clib-cache-authority-lib.c")
      local geometry = t:tmp("lj_t-ffi-clib-cache-geometry.so")
      t:cc(geometry, { t:path("tests", "t-ffi-clib-cache-geometry.c") }, {
        cflags = "-shared -fPIC", link_luajit = false
      })
      local function authority(kind, mode, target, gc)
        local args = { kind, mode, target, library, "helper", gc }
        for _, enabled in ipairs({ false, true }) do
          run_luajit_script(t, "t-ffi-clib-cache-authority.lua", args, {
            joff = not enabled, jon = enabled, timeout = "30s"
          })
        end
      end
      local cases = {
        { "function", "false", "nil", "other", "fenv", "close" },
        { "read", "false", "nil", "other", "close" },
        { "write", "false", "nil", "other", "close" },
        { "zero", "negative-zero", "positive-zero", "false", "nil" },
        { "big", "number", "false", "nil" }
      }
      for _, spec in ipairs(cases) do
        for i = 2, #spec do
          for _, target in ipairs({ "root", "side" }) do
            authority(spec[1], spec[i], target)
          end
        end
      end
      for _, spec in ipairs({
        { "function", "nil" }, { "function", "other" },
        { "function", "fenv" }, { "read", "close" }, { "write", "close" }
      }) do
        for _, target in ipairs({ "root", "side" }) do
          authority(spec[1], spec[2], target, "gc")
        end
      end
      for _, spec in ipairs({
        { "zero", "nan" }, { "zero", "pos-inf" }, { "zero", "neg-inf" },
        { "read", "resize-other" }, { "write", "resize-other" }
      }) do
        for _, target in ipairs({ "root", "side" }) do
          for _, phase in ipairs({ "pre-mt", "mt" }) do
            local args = { spec[1], spec[2], target, library,
              phase == "mt" and "helper" or "no-helper", "gc", phase, geometry }
            for _, enabled in ipairs({ false, true }) do
              run_luajit_script(t, "t-ffi-clib-cache-supplement.lua", args, {
                joff = not enabled, jon = enabled, timeout = "30s"
              })
            end
          end
        end
      end
      local between = t:tmp("lj_t-ffi-clib-cache-between-close")
      t:cc(between, { t:path("tests", "t-ffi-clib-cache-between-close.c") }, {
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread",
                 "-Wl,--wrap=lj_tab_cmpcdata_kgc_rooted_try" }
      })
      for _, gc in ipairs({ false, "gc" }) do
        for _, kind in ipairs({ "read", "write" }) do
          for _, target in ipairs({ "root", "side" }) do
            local args = { between, t:path("tests", "t-ffi-clib-cache-authority.lua"),
              kind, "between-close", target, library }
            if gc then args[#args + 1] = gc end
            t:run(args, { timeout = "30s", env = { LUA_PATH = lua_path(t) } })
          end
        end
      end
      local roots = t:tmp("lj_t-ffi-clib-recorder-roots")
      t:cc(roots, { t:path("tests", "t-ffi-clib-recorder-roots.c") }, {
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread", "-Wl,--wrap=lj_tg_root_anchor_push",
                 "-Wl,--wrap=lj_tab_gettv_rooted_hit_try" }
      })
      for _, mode in ipairs({ "1", "2", "3", "refuse", "hit" }) do
        t:run({ roots, mode }, {
          timeout = "30s", env = { LUA_PATH = lua_path(t) }
        })
      end
      print("M7 namespace cache authority and lifecycle passed")
    end
  })

  add({
    name = "m7_ffi_clib_cdata_compare",
    description = "native cdata cache comparison refusal, overlap and trace retention",
    run = function(t)
      if jit.os ~= "Linux" or jit.arch ~= "x64" then
        print("M7 cdata comparison fixtures require Linux/x64 and GNU ld wrapping")
        return
      end
      build.build_default(t)
      local library = build_shared_library(t,
        t:tmp("lj_t-ffi-clib-cdata-compare.so"),
        "t-ffi-clib-cache-authority-lib.c")
      local flags = table.concat({
        "-DLUA_USE_APICHECK", "-DLUA_USE_ASSERT", "-DLJ_GC2_TEST_HELPERS",
        "-DLJ_FUNC_TEST_HELPERS", "-DLJ_TAB_TEST_HELPERS",
        "-DLJ_ARENA_TEST_HELPERS", "-DLJ_TRACE_TEST_HELPERS",
        "-DLJ_XSAVE_TEST_HELPERS"
      }, " ")
      local function run_comparisons(helpers)
        local binary = t:tmp("lj_t-ffi-clib-cdata-compare-" ..
                             (helpers and "helpers" or "default"))
        t:cc(binary, { t:path("tests", "t-ffi-clib-cdata-compare.c") }, {
          cflags = helpers and flags or "",
          link_luajit = true,
          libs = { "-lm", "-ldl", "-pthread", "-Wl,--export-dynamic",
            "-Wl,--wrap=lj_tab_cmpcdata_kgc_rooted_try",
            "-Wl,--wrap=lj_tg_any_jit_active",
            "-Wl,--wrap=lj_gc2_collect_active",
            "-Wl,--wrap=lj_gc2_smr_read_try",
            "-Wl,--wrap=lj_gc2_smr_read_leave",
            "-Wl,--wrap=lj_gc2_tv_lease_acquire",
            "-Wl,--wrap=lj_gc2_lease_release" }
        })
        local modes = { "wrong", "null", "smr", "gc", "flush" }
        if helpers then
          modes[#modes + 1] = "table-refusal"
          modes[#modes + 1] = "key-refusal"
        end
        for _, mode in ipairs(modes) do
          for _, kind in ipairs({ "function", "read", "write" }) do
            for _, target in ipairs({ "root", "side" }) do
              local args = { binary, t:path("tests", "t-ffi-clib-cdata-probe.lua"),
                kind, mode, target, library }
              if mode == "gc" or mode == "flush" then args[#args + 1] = "gc" end
              t:run(args, { timeout = "38s", env = { LUA_PATH = lua_path(t) } })
            end
          end
        end
        for _, enabled in ipairs({ "on", "off" }) do
          for _, kind in ipairs({ "function", "read", "write" }) do
            for _, target in ipairs({ "root", "side" }) do
              t:run({ binary, t:path("tests", "t-ffi-clib-cdata-retention.lua"),
                kind, "kgc", target, library, enabled }, {
                timeout = "38s", env = { LUA_PATH = lua_path(t) }
              })
            end
          end
        end
      end
      run_comparisons(false)
      build.with_default_build_restore(t, function()
        clean_build(t, { xcflags = flags })
        run_comparisons(true)
      end)
      print("M7 native cdata comparison and trace retention passed")
    end
  })

  add({
    name = "m7_ffi_clib_ldscript",
    description = "FFI C library GNU ld-script resolution behavior",
    run = function(t)
      if jit.os == "OSX" then
        print("M7 FFI clib ld-script behavior skipped on macOS")
        return
      end
      clean_build(t)
      local script = build_clib_ldscript_fixture(t)
      luajit_code(t, [[
local ffi = require("ffi")
ffi.cdef("int lj_clib_ldscript_value(void);")
local cl = ffi.load(assert(os.getenv("LJ_M7_FFI_LDSCRIPT")))
assert(cl.lj_clib_ldscript_value() == 42)
]], {
        env = { LJ_M7_FFI_LDSCRIPT = script }
      })
      print("M7 FFI clib ld-script behavior passed")
    end
  })

  add({
    name = "m7_ffi_nested_state",
    description = "FFI Lua/C API nested lua_State lifecycle behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-nested-state.lua")
      print("M7 FFI nested lua_State lifecycle behavior passed")
    end
  })

  add({
    name = "m7_ffi_cparse_rollback",
    description = "FFI cparser rollback behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-cparse-rollback"),
                      "t-ffi-cparse-rollback.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua")
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, {
        joff = true,
        env = { LJ_M7_FORCE_CTYPE_GROW = "1" }
      })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, {
        env = { LJ_M7_FORCE_CTYPE_GROW = "1" }
      })
      run_luajit_script(t, "t-ffi-cdef-token.lua", { "2", "20" }, { joff = true })
      print("M7 FFI cparser rollback behavior passed")
    end
  })

  add({
    name = "m7_ffi_typeinfo_snapshot",
    description = "FFI ctype metadata snapshot behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-typeinfo-snapshot"),
                      "t-ffi-typeinfo-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-tonumber-snapshot"),
                      "t-ffi-tonumber-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-layout-snapshot"),
                      "t-ffi-layout-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-field-snapshot"),
                      "t-ffi-field-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-element-size-snapshot"),
                      "t-ffi-element-size-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-cconv-init-snapshot"),
                      "t-ffi-cconv-init-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-cdata-conv-snapshot"),
                      "t-ffi-cdata-conv-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-enum-snapshot"),
                      "t-ffi-enum-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-namespace-snapshot"),
                      "t-ffi-namespace-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-istype-snapshot"),
                      "t-ffi-istype-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-tostring-snapshot"),
                      "t-ffi-tostring-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-recorder-string-ctype-busy"),
                      "t-ffi-recorder-string-ctype-busy.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-recorder-libmeta-busy"),
                      "t-ffi-recorder-libmeta-busy.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-recorder-cdata-const-busy"),
                      "t-ffi-recorder-cdata-const-busy.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      print("M7 FFI ctype metadata snapshot behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_hash_publish",
    description = "FFI ctype hash-head behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true, timeout = "20s" })
      run_luajit_script(t, "t-ffi-metatype-miscmap.lua", {
        getenv("LJ_M7_FFI_META_THREADS", "6"),
        getenv("LJ_M7_FFI_META_ITERS", "60")
      }, { joff = true })
      print("M7 FFI ctype hash-head behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_intern_l",
    description = "FFI ctype allocation/interning behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true, timeout = "20s" })
      run_luajit_script(t, "t-ffi-carith-l.lua", nil, { joff = true })
      print("M7 FFI ctype allocation/interning behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_name_claim",
    description = "FFI ctype duplicate-name behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-name-claim"),
                      "t-ffi-ctype-name-claim.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cdef-dup-stack.lua", {
        getenv("LJ_M7_FFI_CDEF_DUP_ROUNDS", "30"),
        getenv("LJ_M7_FFI_CDEF_DUP_ITERS", "200")
      }, { joff = true })
      print("M7 FFI ctype duplicate-name behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_pointer_ids",
    description = "FFI ctype pointer-stability boundary behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-ctype-pointer-ids.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-ctype-pointer-ids.lua", nil, {
        joff = true,
        env = { LJ_M7_FORCE_CTYPE_GROW = "1" }
      })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        "1",
        getenv("LJ_M7_FFI_SET_ITERS", "80")
      }, { joff = true, timeout = "20s" })
      print("M7 FFI ctype pointer-stability behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_tab_retire",
    description = "FFI ctype-table retirement behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-tab-retire"),
                      "t-ffi-ctype-tab-retire.c", { timeout = "20s" })
      print("M7 FFI ctype table-retirement behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_ticket_intern",
    description = "FFI ctype ticket allocation and duplicate-aware interning behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-ticket-intern"),
                      "t-ffi-ctype-ticket-intern.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-ctype-intern-race.lua", {
        getenv("LJ_M7_FFI_INTERN_THREADS", "6"),
        getenv("LJ_M7_FFI_INTERN_SHAPES", "48"),
        getenv("LJ_M7_FFI_INTERN_ROUNDS", "4")
      }, { joff = true })
      print("M7 FFI ctype ticket/intern behavior passed")
    end
  })

  add({
    name = "m7_ffi_finreg",
    description = "FFI cdata finalizer registry behavior",
    run = function(t)
      clean_build(t, { xcflags = "-DLJ_CDATA_TEST_HELPERS" })
      build_and_run_c(t, t:tmp("lj_t-ffi-finreg-clear-races"),
                      "t-ffi-finreg-clear-races.c", {
        build = false,
        cflags = "-DLJ_CDATA_TEST_HELPERS",
        timeout = "30s"
      })
      run_luajit_script(t, "t-ffi-gc-finreg.lua", {
        getenv("LJ_M7_FFI_FIN_THREADS", "6"),
        getenv("LJ_M7_FFI_FIN_ITERS", "240")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-gc-finreg.lua", {
        getenv("LJ_M7_FFI_FIN_THREADS", "6"),
        getenv("LJ_M7_FFI_FIN_ITERS", "240")
      })
      run_luajit_script(t, "t-ffi-gc-trace.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })
      build_and_run_c(t, t:tmp("lj_t-ffi-finreg-free-path"),
                      "t-ffi-finreg-free-path.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-finreg-slot-lease"),
                      "t-ffi-finreg-slot-lease.c",
                      { build = false, timeout = "20s" })
      print("M7 FFI finalizer registry behavior passed")
    end
  })

  add({
    name = "m7_ffi_jit_cnew",
    description = "x64 JIT CNEW/CNEWI cdata publication behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-jit-cnew-alloc.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })

      luajit_code(t, [[
local ffi = require"ffi"
local util = require"jit.util"
ffi.cdef"typedef struct { int x; double y; } lj_m7_jit_cnew_t;"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local struct_t = ffi.typeof("lj_m7_jit_cnew_t")
local int64_t = ffi.typeof("int64_t")
local function make(n)
  local sum = 0
  for i = 1, n do
    local obj = struct_t(i, i + 0.25)
    local i64 = int64_t(i)
    sum = sum + obj.x + tonumber(i64)
  end
  return sum
end
for _ = 1, 30 do assert(make(80) == 6480) end
assert(util.traceinfo(1), "CNEW allocation loop did not trace")
print("jit cnew ok")
]])
      luajit_code(t, [[
local ffi = require"ffi"
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local int64_t = ffi.typeof("int64_t")
local function make(n)
  local v = int64_t(0)
  for i = 1, n do v = int64_t(i) end
  return v
end
for _ = 1, 30 do assert(tonumber(make(80)) == 80) end
assert(util.traceinfo(1), "CNEWI allocation loop did not trace")
print("jit cnewi ok")
]])
      luajit_code(t, [[
local ffi = require"ffi"
local util = require"jit.util"
ffi.cdef"typedef struct { uint8_t x[1024]; } lj_m7_jit_cnew_big_t;"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local big_t = ffi.typeof("lj_m7_jit_cnew_big_t")
local sink
local function make(n)
  for i = 1, n do
    local obj = big_t()
    obj.x[0] = i
    sink = obj
  end
  return sink.x[0]
end
for _ = 1, 30 do assert(make(80) == 80) end
assert(util.traceinfo(1), "large CNEW allocation loop did not trace")
print("jit big cnew ok")
]])

      clean_build(t, build.gc2_paranoia_opts())
      run_stock(t, { "test.lua", "--quiet", "lib/ffi/jit_struct.lua" }, {
        timeout = "20s"
      })
      run_stock(t, { "test.lua", "--quiet", "lib/ffi/type_punning.lua" }, {
        timeout = "20s"
      })
      print("M7 FFI JIT CNEW allocation behavior passed")
    end
  })

  add({
    name = "m7_ffi_metatype",
    description = "FFI metatype side-map behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-metatype-snapshot"),
                      "t-ffi-metatype-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-metatv-snapshot"),
                      "t-ffi-metatv-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-recorder-metatv-busy"),
                      "t-ffi-recorder-metatv-busy.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-metatype-miscmap.lua", {
        getenv("LJ_M7_FFI_META_THREADS", "6"),
        getenv("LJ_M7_FFI_META_ITERS", "60")
      }, { joff = true })
      print("M7 FFI metatype/miscmap behavior passed")
    end
  })

  add({
    name = "m7_ffi_snap_restore_l",
    description = "FFI snapshot restore cdata allocation behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-snap-restore-l.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })
      build_and_run_c(t, t:tmp("lj_t-ffi-snap-restore-snapshot"),
                      "t-ffi-snap-restore-snapshot.c", {
        build = false,
        timeout = "20s"
      })
      print("M7 FFI snapshot restore cdata allocation behavior passed")
    end
  })

  add({
    name = "m7_ffi",
    description = "M7 FFI aggregate concurrency gates",
    deps = m7_cases,
    run = function(t)
      runtime.run_lua_test_cases(t, m7_cases)
      print("M7 FFI gates passed")
    end
  })
end
