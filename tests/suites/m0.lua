local runtime = require("suite_runtime")
local checks = require("suite_assert")
local utils = require("suite_utils")

local assert_file_contains = checks.assert_file_contains
local write_file = utils.write_file
local with_temp_paths = utils.with_temp_paths

local function assert_rejected(label, fn)
  local ok = pcall(fn)
  if ok then error(label .. " was not rejected", 2) end
end

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
    name = "m0_source_guard",
    description = "test framework source-read guard behavior",
    run = function(t)
      assert_rejected("src read", function()
        t:read(t:path("src", "lj_tab.c"))
      end)
      assert_rejected("suite utils source read", function()
        utils.read_file(t:path("src", "lj_tab.c"))
      end)
      assert_rejected("suite source assertion", function()
        assert_file_contains(t, t:path("tests", "suites", "m0.lua"), "__never__")
      end)
      assert_rejected("wrapper source assertion", function()
        assert_file_contains(t, t:path("tools", "ci", "m0_matrix.sh"), "__never__")
      end)
      with_temp_paths(t, { "lj-source-guard-result" }, function(result)
        write_file(result, "generated result marker\n")
        assert_file_contains(t, result, "generated result marker",
                             "generated result file")
        assert(t:read(result):match("generated result marker"))
      end)
      print("M0 source guard behavior passed")
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
