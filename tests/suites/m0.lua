local runtime = require("suite_runtime")

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
    name = "m0_matrix",
    description = "M0 default and no-JIT stock-test build matrix",
    run = function(t)
      run_m0_combo(t, "lockless JIT=1", "", {})
      run_m0_combo(t, "lockless JIT=0", "-DLUAJIT_DISABLE_JIT", { "-jit" })
      print("M0 matrix passed")
    end
  })
end
