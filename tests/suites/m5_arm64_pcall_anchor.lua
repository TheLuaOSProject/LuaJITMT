local build = require("suite_build")

local bootstrap_cflags =
  "-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT"

local function native_bootstrap()
  local ok, jitmod = pcall(require, "jit")
  return ok and jitmod and jitmod.os == "OSX" and jitmod.arch == "arm64" and
         jitmod.status() == false and jitmod.opt == nil
end

local function skip(name)
  print(name .. " SKIP: requires a native macOS arm64 disabled-JIT bootstrap build")
end

local function run_contract(t)
  t:run({ "sh", t:path("tools", "ci",
                        "arm64_pcall_anchor_contract.sh") }, {
    timeout = "30s"
  })
end

return function(add)
  add({
    name = "m5_arm64_pcall_anchor_contract",
    description = "ARM64 fast-protected-call checkpoint source/object contract",
    run = function(t)
      run_contract(t)
    end
  })

  add({
    name = "m5_arm64_pcall_anchor_runtime",
    description = "ARM64 nonzero-anchor pcall/xpcall unwind and tail fallback",
    deps = { "m5_arm64_pcall_anchor_contract" },
    run = function(t)
      if not native_bootstrap() then
        skip("m5_arm64_pcall_anchor_runtime")
        return
      end
      run_contract(t)  -- The fixture links the exact inspected archive.
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-pcall-anchor"),
                              "t-arm64-pcall-anchor.c", {
        cflags = bootstrap_cflags,
        quiet = true,
        timeout = "30s"
      })
    end
  })
end
