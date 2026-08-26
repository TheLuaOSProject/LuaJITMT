local build = require("suite_build")

local bootstrap_cflags =
  "-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT"
local fail_closed_cflags =
  "-DLUAJIT_MT_ARM64_BOOTSTRAP " ..
  "-DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT " ..
  "-DLJ_TRACE_TEST_HELPERS"

local function native_bootstrap()
  local ok, jitmod = pcall(require, "jit")
  return ok and jitmod and jitmod.os == "OSX" and jitmod.arch == "arm64" and
         jitmod.status() == false and jitmod.opt == nil
end

local function native_fail_closed_jit()
  local ok, jitmod = pcall(require, "jit")
  local util_ok, util = pcall(require, "jit.util")
  return ok and jitmod and util_ok and util and jitmod.os == "OSX" and
         jitmod.arch == "arm64" and jitmod.status() == true and
         util.traceinfo(1) == nil
end

local function skip(name)
  print(name .. " SKIP: requires a native macOS arm64 disabled-JIT bootstrap build")
end

local function run_contract(t, source_only)
  t:run({ "sh", t:path("tools", "ci",
                        "arm64_vm_safepoint_contract.sh") }, {
    env = source_only and { LJ_ARM64_SAFEPOINT_SOURCE_ONLY = "1" } or nil,
    timeout = "30s"
  })
end

return function(add)
  add({
    name = "m5_arm64_safepoint_contract",
    description = "ARM64 VM safepoint source, object and archive contract",
    run = function(t)
      run_contract(t)
    end
  })

  add({
    name = "m5_arm64_safepoint_runtime",
    description = "ARM64 interpreter progress, entry and exit safepoint regression",
    deps = { "m5_arm64_safepoint_contract" },
    run = function(t)
      if not native_bootstrap() then
        skip("m5_arm64_safepoint_runtime")
        return
      end
      -- Recheck freshness because this fixture links the inspected archive.
      run_contract(t)
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-vm-safepoint"),
                              "t-vm-safepoint.c", {
        cflags = bootstrap_cflags,
        quiet = true,
        timeout = "30s"
      })
    end
  })

  add({
    name = "m5_arm64_jit_fail_closed_safepoint_runtime",
    description = "ARM64 JIT admission keeps interpreter safepoints live with zero traces",
    deps = { "m5_arm64_safepoint_contract" },
    run = function(t)
      if not native_fail_closed_jit() then
        print("m5_arm64_jit_fail_closed_safepoint_runtime SKIP: " ..
              "requires the native experimental ARM64 fail-closed JIT build")
        return
      end
      run_contract(t, true)
      t:run({ "sh", t:path("tools", "ci",
                            "jit_recorder_safepoint_contract.sh") },
            { timeout = "15s" })
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-jit-fail-closed-safepoint"),
                              "t-vm-safepoint.c", {
        cflags = fail_closed_cflags,
        quiet = true,
        timeout = "30s"
      })
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-jit-recorder-safepoint"),
                              "t-jit-recorder-safepoint.c", {
        cflags = fail_closed_cflags,
        quiet = true,
        timeout = "30s"
      })
    end
  })
end
