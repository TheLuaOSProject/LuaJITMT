local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")
local utils = require("suite_utils")

local capture_luajit = runtime.capture_luajit
local luajit_script = runtime.luajit_script
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local shell_quote = utils.shell_quote
local gc2_test_cflags = "-DLJ_GC2_TEST_HELPERS"

local M8_C_FIXTURES = {
  "t-gc2-phase",
  "t-gc2-traverse",
  "t-m8-ffi-weak-newindex",
  "t-m8-close-finalizers",
  "t-m8-finalizer-state"
}

local function assert_finalizer_claim_cleanup_boundaries(t)
  local awk_helper = [=[
BEGIN { infn = 0; grow = 0; drop = 0; report = 0 }
/^static int gc2_finalizer_checkstack_claimed\(global_State \*g,/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_state_cpgrowstack/ { grow = NR }
infn && /lj_state_dropclaim/ { drop = NR }
infn && /lj_vmevent_send/ {
  report = NR
  if (!drop || drop > NR) {
    print "finalizer stack-prep error reports before dropping state claim"
    exit 1
  }
}
END {
  if (!grow || !drop || !report) {
    print "finalizer stack-prep claim cleanup helper missing"
    exit 1
  }
}
]=]
  local awk_call = [=[
BEGIN { infn = 0; claim = 0; check = 0; hook = 0 }
/^static int gc2_call_finalizer\(global_State \*g, lua_State \*L,/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_state_tryclaim/ { claim = NR }
infn && /gc2_finalizer_checkstack_claimed/ { check = NR }
infn && /hook_entergc/ {
  hook = NR
  if (!check || check > NR) {
    print "finalizer stack prep is not checked before hook/threshold mutation"
    exit 1
  }
}
END {
  if (!claim || !check || !hook || claim > check) {
    print "finalizer claim/checkstack boundary missing"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(awk_helper) ..
                        " src/lj_gc2.c")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(awk_call) ..
                        " src/lj_gc2.c")
end

local function finalizer_error_native_stdio_smoke()
  return [=[
local ffi = require("ffi")
ffi.cdef("typedef struct { int x; } lj_finalizer_error_native_stdio_t;")
do
  local x = ffi.gc(ffi.new("lj_finalizer_error_native_stdio_t"), function()
    error("finalizer native stdio smoke")
  end)
end
collectgarbage("collect")
collectgarbage("collect")
]=]
end

local function run_finalizer_error_native_stdio(t, opts)
  opts = opts or {}
  local out = t:tmp("lj_m8_finalizer_error_native_stdio.out")
  assert_finalizer_claim_cleanup_boundaries(t)
  if opts.build ~= false then
    t:build({ quiet = true })
  end
  capture_luajit(t, { "-e", finalizer_error_native_stdio_smoke() }, out, {
    stderr_to_stdout = true
  })
  checks.assert_text_all_contains("finalizer error native stdio output",
                                  t:read(out), {
    "ERROR in finalizer: ",
    "finalizer native stdio smoke"
  }, "captured output")
end

local function run_default_matrix(t)
  t:build({ clean = true, quiet = true })
  run_finalizer_error_native_stdio(t, { build = false })
  run_luajit_script_jit_modes(t, "t-weak-modes.lua")
  run_luajit_script_jit_modes(t, "t-m8-finalizer-spawn-live.lua", nil,
                              { timeout = "10s" })
  run_luajit_script_jit_modes(t, "t-ffi-gc-finreg.lua", { "3", "72" })
  build.run_c_fixtures(t, M8_C_FIXTURES, {
    output_suffix = "_m8",
    cflags = gc2_test_cflags,
    timeout = "20s"
  })
end

local function run_paranoia_matrix(t)
  local xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
  t:build({ clean = true, quiet = true, xcflags = xcflags })
  luajit_script(t, "t-weak-modes.lua", nil, {
    joff = true,
    env = {
      LJ_M8_WEAK_RACE_ITERS = "0",
      LJ_M8_FINALIZER_SPAWN = "0"
    }
  })
  build.run_c_fixtures(t, M8_C_FIXTURES, {
    output_suffix = "_m8_paranoia",
    cflags = xcflags,
    timeout = "20s"
  })
end

return function(add)
  add({
    name = "m8_finalizer_error_native_stdio",
    description = "default finalizer error reporter uses native stdio boundary",
    run = function(t)
      run_finalizer_error_native_stdio(t)
      print("M8 finalizer error native stdio behavior passed")
    end
  })

  add({
    name = "m8_weak",
    description = "M8 weak-table/finalizer semantic gates",
    run = function(t)
      run_default_matrix(t)
      run_paranoia_matrix(t)
      print("M8 weak/finalizer semantic gates passed")
    end
  })
end
