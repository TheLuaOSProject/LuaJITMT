local runtime = require("suite_runtime")
local build = require("suite_build")

local function run_m0_combo(t, name, xcflags, stock_tags)
  print("== " .. name .. " ==")
  t:build({ clean = true, xcflags = xcflags })

  local stock_args = { "test.lua", "--quiet" }
  for i = 1, #stock_tags do stock_args[#stock_args + 1] = stock_tags[i] end
  runtime.run_stock(t, stock_args, {
    bin = t:path("src", "luajit"),
    check_executable = true
  })

  runtime.luajit_code(t, "require'ffi'; assert(2^31 == 2147483648)")
end

return function(add)
  add({
    name = "run_stock_tests",
    description = "vendored LuaJIT stock cleanup suite",
    run = function(t, args)
      runtime.run_stock_cli(t, args)
    end
  })

  add({
    name = "m0_build_profile_switch",
    description = "Lua test harness rebuilds when the XCFLAGS profile changes",
    run = function(t)
      build.build_default(t, {
        quiet = true,
        jobs = false,
        args = { "XCFLAGS=-DLUAJIT_DISABLE_JIT" }
      })
      t.build_signature = nil
      build.build_default(t, { quiet = true, jobs = false })
      runtime.luajit_code(t, "assert(jit.status())", { quiet = true })
      print("M0 build profile switch passed")
    end
  })

  add({
    name = "m0_matrix",
    description = "M0 default and no-JIT stock-test build matrix",
    run = function(t)
      run_m0_combo(t, "lockless JIT=1", "", {})
      run_m0_combo(t, "lockless JIT=0", "-DLUAJIT_DISABLE_JIT", { "-jit" })
      print("M0 matrix passed")
    end
  })

  add({
    name = "m0_lua52_compat",
    description = "optional stock Lua 5.2 compatibility build profile",
    run = function(t)
      run_m0_combo(t, "lockless Lua 5.2 compat",
                   "-DLUAJIT_ENABLE_LUA52COMPAT", {})
      runtime.luajit_code(t, [[
        assert(table.pack and table.unpack == unpack)
        local packed = table.pack("x", nil)
        assert(packed.n == 2 and packed[1] == "x" and packed[2] == nil)
        assert(rawlen({ 1, 2, 3 }) == 3)
        local _, ismain = coroutine.running()
        assert(type(ismain) == "boolean")
        local marker = {}
        local iter = pairs(setmetatable({}, {
          __pairs = function()
            local done
            return function()
              if done then return nil end
              done = true
              return marker, 42
            end
          end
        }))
        local k, v = iter()
        assert(k == marker and v == 42)
      ]])
      print("M0 Lua 5.2 compatibility build passed")
    end
  })
end
