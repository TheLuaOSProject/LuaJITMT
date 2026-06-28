local utils = require("suite_utils")
local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")
local jitutils = require("suite_jit")

local getenv = utils.getenv
local assert_dump_contains = checks.assert_dump_contains
local lua_path = runtime.lua_path
local build_and_run_c = build.compile_and_run_c
local run_c_fixture_specs = build.run_c_fixture_specs
local build_shared_library = build.build_shared_library
local write_ld_script = build.write_ld_script
local clean_build = build.clean_build
local luajit_dump_file = runtime.luajit_dump_file
local luajit_code = runtime.luajit_code
local luajit_file = runtime.luajit_file
local run_luajit_script = runtime.luajit_script
local run_stock = runtime.run_stock
local run_ir_dump_probe = jitutils.run_ir_dump_probe

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
  "m7_ffi_pin",
  "m7_ffi_metatype",
  "m7_ffi_cdata_get_l",
  "m7_ffi_cdata_set_l",
  "m7_ffi_carith_l",
  "m7_ffi_clib_cache",
  "m7_ffi_clib_ldscript",
  "m7_ffi_nested_state",
  "m7_ffi_callback_install",
  "m7_ffi_callback_runtime",
  "m7_ffi_blocking"
}

local function build_clib_ldscript_fixture(t)
  local script = t:tmp("lj_t-ffi-clib-ldscript.so")
  local so = build_shared_library(t, t:tmp("lj_t-ffi-clib-ldscript-real.so"),
                                  "t-ffi-clib-ldscript-lib.c")
  return write_ld_script(script, so)
end

return function(add)
  add({
    name = "m7_ffi_blocking",
    description = "FFI blocking recorder blacklist behavior",
    run = function(t)
      clean_build(t)
      run_c_fixture_specs(t, {
        { output = "lj_t-ffi-cbblack-race",
          cfile = "t-ffi-cbblack-race.c" },
        { output = "lj_t-ffi-ccall-native-helpers",
          cfile = "t-ffi-ccall-native-helpers.c" },
        { output = "lj_t-ffi-ccall-stopreq",
          cfile = "t-ffi-ccall-stopreq.c" }
      })
      run_luajit_script(t, "t-ffi-blocking.lua")
      print("M7 ffi.blocking behavior passed")
    end
  })

  add({
    name = "m7_ffi_callback_install",
    description = "FFI callback slot install behavior",
    run = function(t)
      clean_build(t)
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
          cfile = "t-ffi-callback-auto-attach.c" }
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
      run_luajit_script(t, "t-ffi-carith-l.lua", nil, { joff = true })
      print("M7 FFI arithmetic/raw conversion behavior passed")
    end
  })

  add({
    name = "m7_ffi_cdata_alloc",
    description = "concurrent FFI cdata allocation behavior",
    run = function(t)
      clean_build(t)
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
      }, { joff = true })
      print("M7 FFI cdata write behavior passed")
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
      clean_build(t)
      run_luajit_script(t, "t-ffi-clib-cache.lua", {
        getenv("LJ_M7_FFI_CLIB_THREADS", "6"),
        getenv("LJ_M7_FFI_CLIB_ITERS", "300")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-clib-cache.lua", {
        getenv("LJ_M7_FFI_CLIB_JIT_THREADS", "2"),
        getenv("LJ_M7_FFI_CLIB_JIT_ITERS", "180")
      })
      print("M7 FFI clib cache behavior passed")
    end
  })

  add({
    name = "m7_ffi_clib_ldscript",
    description = "FFI C library GNU ld-script resolution behavior",
    run = function(t)
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

      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
      build_and_run_c(t, t:tmp("lj_t-ffi-cparse-rollback-anchor"),
                      "t-ffi-cparse-rollback.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua")
      run_luajit_script(t, "t-ffi-cdef-token.lua", { "2", "20" }, { joff = true })
      print("M7 FFI cparser rollback behavior passed")
    end
  })

  add({
    name = "m7_ffi_typeinfo_snapshot",
    description = "FFI typeinfo ctype snapshot behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-typeinfo-snapshot"),
                      "t-ffi-typeinfo-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-layout-snapshot"),
                      "t-ffi-layout-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-field-snapshot"),
                      "t-ffi-field-snapshot.c", { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-ffi-element-size-snapshot"),
                      "t-ffi-element-size-snapshot.c", { timeout = "20s" })
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
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      print("M7 FFI typeinfo snapshot behavior passed")
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
      }, { joff = true })
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
      }, { joff = true })
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
      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
      run_luajit_script(t, "t-ffi-ctype-pointer-ids.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        "1",
        getenv("LJ_M7_FFI_SET_ITERS", "80")
      }, { joff = true })
      print("M7 FFI ctype pointer-stability behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_tab_retire",
    description = "FFI ctype-table retirement behavior",
    run = function(t)
      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
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
      clean_build(t)
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
      local dump = t:tmp("lj_t-ffi-gc-trace.dump")
      luajit_dump_file(t, dump, "-jdump=ir",
                        t:path("tests", "t-ffi-gc-trace.lua"), nil, {
        timeout = "20s",
        stderr = false
      })
      assert_dump_contains(t, dump, "lj_cdata_setfin", "FFI finalizer trace")
      build_and_run_c(t, t:tmp("lj_t-ffi-finreg-free-invariant"),
                      "t-ffi-finreg-free-invariant.c", { timeout = "20s" })
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

      local dump = t:tmp("lj_t-ffi-jit-cnew.dump")
      local dumpi = t:tmp("lj_t-ffi-jit-cnewi.dump")
      run_ir_dump_probe(t, dump, [[
local ffi = require"ffi"
ffi.cdef"typedef struct { int x; double y; } lj_m7_jit_dump_t;"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local struct_t = ffi.typeof("lj_m7_jit_dump_t")
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
print("dump cnew ok")
]])
      run_ir_dump_probe(t, dumpi, [[
local ffi = require"ffi"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local int64_t = ffi.typeof("int64_t")
local function make(n)
  local v = int64_t(0)
  for i = 1, n do v = int64_t(i) end
  return v
end
for _ = 1, 30 do assert(tonumber(make(80)) == 80) end
print("dump cnewi ok")
]])
      assert_dump_contains(t, dump, "CNEW", "CNEW trace")
      assert_dump_contains(t, dumpi, "CNEWI", "CNEWI trace")
      assert_dump_contains(t, dump, "dump cnew ok", "CNEW probe")
      assert_dump_contains(t, dumpi, "dump cnewi ok", "CNEWI probe")

      clean_build(t, { xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" })
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
      run_luajit_script(t, "t-ffi-metatype-miscmap.lua", {
        getenv("LJ_M7_FFI_META_THREADS", "6"),
        getenv("LJ_M7_FFI_META_ITERS", "60")
      }, { joff = true })
      print("M7 FFI metatype/miscmap behavior passed")
    end
  })

  add({
    name = "m7_ffi_pin",
    description = "ffi.pin root publication and release behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-pin.lua", {
        getenv("LJ_M7_FFI_PIN_THREADS", "4"),
        getenv("LJ_M7_FFI_PIN_ITERS", "80")
      }, { joff = true })
      print("M7 ffi.pin behavior passed")
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
