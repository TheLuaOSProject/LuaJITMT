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

return function(add)
  add({
    name = "m5_arm64_root_publication_contract",
    description = "ARM64 VM stack/root publication source and object contract",
    run = function(t)
      t:run({ "sh", t:path("tools", "ci",
                           "arm64_root_publication_contract.sh") }, {
        timeout = "30s"
      })
    end
  })

  add({
    name = "m5_arm64_root_publication_runtime",
    description = "ARM64 interpreter opcode semantics and root-retention regression",
    run = function(t)
      if not native_bootstrap() then
        skip("m5_arm64_root_publication_runtime")
        return
      end
      build.compile_and_run_c(t, t:tmp("lj_t-arm64-root-publication"),
                              "t-arm64-root-publication.c", {
        cflags = bootstrap_cflags,
        quiet = true,
        timeout = "45s"
      })
    end
  })
end
