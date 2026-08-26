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
                        "arm64_meta_publication_contract.sh") }, {
    timeout = "30s"
  })
end

return function(add)
  add({
    name = "m5_arm64_meta_publication_contract",
    description = "ARM64 metamethod/metatable source and object contract",
    run = function(t)
      run_contract(t)
    end
  })

  add({
    name = "m5_arm64_meta_publication_runtime",
    description = "ARM64 metamethod/metatable semantics and root retention",
    deps = { "m5_arm64_meta_publication_contract" },
    run = function(t)
      if not native_bootstrap() then
        skip("m5_arm64_meta_publication_runtime")
        return
      end
      -- Standalone runtime selection must never link a stale archive. The
      -- contract checks source/object/archive/executable freshness first.
      run_contract(t)
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-meta-publication"),
                              "t-arm64-meta-publication.c", {
        cflags = bootstrap_cflags,
        env = { LJ_TEST_ROOT = t.root },
        quiet = true,
        timeout = "45s"
      })
    end
  })
end
