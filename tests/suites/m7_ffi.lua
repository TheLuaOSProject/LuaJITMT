local utils = require("suite_utils")

local getenv = utils.getenv
local shell_quote = utils.shell_quote

local function lua_path(t)
  return t:path("src", "?.lua") .. ";" .. t:path("src", "jit", "?.lua") .. ";;"
end

local function build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = opts.timeout })
end

local function clean_build(t, opts)
  opts = opts or {}
  t:build({ clean = true, quiet = true, xcflags = opts.xcflags })
end

local function run_luajit_script(t, script, args, opts)
  args = args or {}
  opts = opts or {}
  local argv = {}
  if opts.joff then argv[#argv + 1] = "-joff" end
  argv[#argv + 1] = t:path("tests", script)
  for i = 1, #args do argv[#argv + 1] = args[i] end
  t:luajit(argv, { timeout = opts.timeout, env = opts.env })
end

local function run_dump_probe(t, dump, script)
  local cmd = "LUA_PATH=" .. shell_quote(lua_path(t)) .. " " ..
              "timeout 20s " .. shell_quote(t:path("src", "luajit")) ..
              " -jdump=ir -e " .. shell_quote(script) ..
              " >" .. shell_quote(dump)
  t:run(cmd)
end

local m7_cases = {
  "m7_ffi_cdef_token",
  "m7_ffi_cdef_dup_stack",
  "m7_ffi_cparse_rollback",
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
  "m7_ffi_callback_install",
  "m7_ffi_callback_runtime",
  "m7_ffi_blocking"
}

return function(add)
  add({
    name = "m7_ffi_blocking",
    description = "FFI blocking recorder blacklist behavior",
    run = function(t)
      clean_build(t)
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
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-nested-native"),
                      "t-ffi-callback-nested-native.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-owner-lifetime"),
                      "t-ffi-callback-owner-lifetime.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-attached-carrier"),
                      "t-ffi-callback-attached-carrier.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-auto-attach"),
                      "t-ffi-callback-auto-attach.c")
      run_luajit_script(t, "t-ffi-callback-runtime.lua", {
        getenv("LJ_M7_FFI_CBACK_RT_THREADS", "6"),
        getenv("LJ_M7_FFI_CBACK_RT_ITERS", "220")
      }, { joff = true })
      t:luajit({ t:path("tests", "stock", "test", "lib", "ffi", "ffi_callback.lua") })
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
      t:run("LUA_PATH=" .. shell_quote(lua_path(t)) .. " timeout 20s " ..
            shell_quote(t:path("src", "luajit")) .. " -jdump=ir " ..
            shell_quote(t:path("tests", "t-ffi-gc-trace.lua")) ..
            " >" .. shell_quote(dump))
      t:assert_contains(dump, "lj_cdata_setfin")
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
      run_dump_probe(t, dump, [[
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
      run_dump_probe(t, dumpi, [[
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
      t:assert_contains(dump, "CNEW")
      t:assert_contains(dumpi, "CNEWI")
      t:assert_contains(dump, "dump cnew ok")
      t:assert_contains(dumpi, "dump cnewi ok")

      clean_build(t, { xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" })
      t:run("(cd " .. shell_quote(t:path("tests", "stock", "test")) ..
            " && LUA_PATH=" .. shell_quote(lua_path(t)) ..
            " timeout 20s " .. shell_quote(t:path("src", "luajit")) ..
            " test.lua --quiet 340 341 358)")
      print("M7 FFI JIT CNEW allocation behavior passed")
    end
  })

  add({
    name = "m7_ffi_metatype",
    description = "FFI metatype side-map behavior",
    run = function(t)
      clean_build(t)
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
    run = function(t)
      local cmd = { t:path("tools", "ci", "lua_test.sh") }
      for i = 1, #m7_cases do cmd[#cmd + 1] = m7_cases[i] end
      t:run(cmd)
      print("M7 FFI gates passed")
    end
  })
end
