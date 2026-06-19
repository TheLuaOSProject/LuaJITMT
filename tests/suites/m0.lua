local utils = require("suite_utils")

local function quote(s)
  return utils.shell_quote(s)
end

local function stock_lua_path(t)
  return utils.lua_path(t.root)
end

local function run_stock(t, args)
  args = args or {}
  local bin = args[1] or t:path("src", "luajit")
  if bin:sub(1, 1) ~= "/" then bin = t:path(bin) end
  t:run({ "test", "-x", bin }, { quiet = true })

  local cmd = "cd " .. quote(t:path("tests", "stock", "test")) ..
              " && LUA_PATH=" .. quote(stock_lua_path(t)) .. " " ..
              quote(bin) .. " test.lua"
  for i = 2, #args do cmd = cmd .. " " .. quote(args[i]) end
  t:run(cmd)
end

local function run_m0_combo(t, name, xcflags, stock_tags)
  print("== " .. name .. " ==")
  t:build({ clean = true, xcflags = xcflags })

  local stock_args = { t:path("src", "luajit"), "--quiet" }
  for i = 1, #stock_tags do stock_args[#stock_args + 1] = stock_tags[i] end
  run_stock(t, stock_args)

  t:luajit({ "-e", "require'ffi'; assert(2^31 == 2147483648)" })
end

return function(add)
  add({
    name = "run_stock_tests",
    description = "vendored LuaJIT stock cleanup suite",
    run = function(t, args)
      run_stock(t, args)
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
