local build = require("suite_build")
local runtime = require("suite_runtime")

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

local function run_tmpbuf_contract(t)
  t:run({ "sh", t:path("tools", "ci", "arm64_tmpbuf_contract.sh") }, {
    timeout = "30s"
  })
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

  add({
    name = "m5_arm64_tmpbuf_contract",
    description = "ARM64 string fast functions use the running TG tmpbuf",
    run = function(t)
      run_tmpbuf_contract(t)
    end
  })

  add({
    name = "m5_arm64_vm_next_contract",
    description = "ARM64 JIT lj_vm_next forwarding and PAC contract",
    run = function(t)
      t:run({ "sh", t:path("tools", "ci",
                           "arm64_jit_vm_next_contract.sh") }, {
        env = { LJ_TEST_RUN_LOCK_HELD = "1" },
        timeout = "180s"
      })
    end
  })

  add({
    name = "m5_arm64_tmpbuf_runtime",
    description = "ARM64 concurrent reverse/lower/upper TG tmpbuf regression",
    deps = { "m5_arm64_tmpbuf_contract" },
    run = function(t)
      if not native_bootstrap() then
        skip("m5_arm64_tmpbuf_runtime")
        return
      end
      -- Recheck freshness because this script exercises the inspected VM.
      run_tmpbuf_contract(t)
      runtime.luajit_script(t, "t-arm64-tmpbuf-thread.lua", nil, {
        joff = true,
        timeout = "90s"
      })
    end
  })
end
