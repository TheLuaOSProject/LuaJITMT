local build = require("suite_build")

local bootstrap_flags =
  "-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT"
local flags = bootstrap_flags .. " " ..
  "-DLJ_API_ROOT_TEST_HELPERS -DLJ_TG_ROOT_TEST_HELPERS"

local function native_arm64_macos()
  local ok, jitmod = pcall(require, "jit")
  return ok and jitmod and jitmod.os == "OSX" and jitmod.arch == "arm64"
end

local function with_bootstrap_restore(t, fn)
  local ok, err = xpcall(fn, debug.traceback)
  local restore_ok, restore_err = xpcall(function()
    t:build({ clean = true, quiet = true, xcflags = bootstrap_flags })
  end, debug.traceback)
  if not ok then
    if not restore_ok then
      err = err .. "\n\n(ARM64 bootstrap restore also failed)\n" ..
            restore_err
    end
    error(err, 0)
  end
  if not restore_ok then error(restore_err, 0) end
end

return function(add)
  add({
    name = "m5_arm64_capi_meta_roots",
    description = "ARM64 rooted C API equality, comparison and metatable paths",
    run = function(t)
      if not native_arm64_macos() then
        print("m5_arm64_capi_meta_roots SKIP: requires native macOS arm64")
        return
      end
      with_bootstrap_restore(t, function()
        t:build({ clean = true, quiet = true, xcflags = flags })
        build.compile_and_run_c(t, t:tmp("lj_t-arm64-capi-meta-roots"),
                                "t-arm64-capi-meta-roots.c", {
          cflags = flags,
          env = { LJ_TEST_ROOT = t.root },
          quiet = true,
          timeout = "60s"
        })
      end)
    end
  })
end
