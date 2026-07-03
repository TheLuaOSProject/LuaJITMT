local build = require("suite_build")
local runtime = require("suite_runtime")

local build_and_run_luajit_code = runtime.build_and_run_luajit_code
local compile_and_run_c = build.compile_and_run_c

local function tget_array_header_smoke()
  return [[
local t = {}
for i = 1, 96 do t[i] = i * 3 end
assert(t[64] == 192)
local k = 70
assert(t[k] == 210)
local function getv(a, key) return a[key] end
assert(getv(t, 80) == 240)
assert(t[120] == nil)
]]
end

local function tgets_node_order_smoke()
  return [[
local t = { foo = 17 }
local sum = 0
for i = 1, 200 do
  sum = sum + t.foo
  t.bar = i
  assert(t.bar == i)
end
assert(sum == 200 * 17)
]]
end

local function getmetatable_node_order_smoke()
  return [[
local token = {}
local t = setmetatable({}, { __metatable = token })
assert(getmetatable(t) == token)
local u = setmetatable({}, {})
assert(type(getmetatable(u)) == "table")
local v = {}
local mt = { tag = "plain" }
assert(setmetatable(v, mt) == v)
assert(getmetatable(v) == mt)
x64_vm_gcref_edge = nil
x64_vm_gcref_edge = 41
assert(x64_vm_gcref_edge == 41 and _G.x64_vm_gcref_edge == 41)
_G.x64_vm_gcref_edge = nil
assert(type(tostring(12.5)) == "string")
]]
end

local function ipairs_snapshot_smoke()
  return [[
local t = { 10, 20, nil, 40 }
local n, sum = 0, 0
for i, v in ipairs(t) do
  n = n + 1
  sum = sum + i + v
end
assert(n == 2 and sum == 33)
]]
end

local function itern_snapshot_smoke()
  return [[
local t = { [0] = "z", "a", nil, "c", alpha = 11, beta = 12 }
local seen, n = {}, 0
for k, v in pairs(t) do
  seen[k] = v
  n = n + 1
end
assert(n == 5)
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.alpha == 11 and seen.beta == 12)
]]
end

local function table_next_snapshot_smoke()
  return [[
local t = { [0] = "z", "a", nil, "c", x = 41, y = 42 }
local seen = {}
for k, v in next, t, nil do seen[k] = v end
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.x == 41 and seen.y == 42)
local n = 0
for k, v in pairs(t) do
  assert(seen[k] == v)
  n = n + 1
end
assert(n == 5)
]]
end

return function(add)
  add({
    name = "m5_x64_tnew_empty_inline",
    description = "x64 interpreter empty TNEW inline arena bump behavior",
    run = function(t)
      build.build_and_run_c(t, t:tmp("lj_t-x64-tnew-empty-inline"),
                            "t-x64-tnew-empty-inline.c", {
        clean = true,
        cflags = "-DLJ_TAB_TEST_HELPERS",
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      print("M5 x64 empty TNEW inline arena bump behavior passed")
    end
  })

  add({
    name = "m5_x64_getmetatable_node_order",
    description = "x64 getmetatable behavior",
    run = function(t)
      build_and_run_luajit_code(t, getmetatable_node_order_smoke(),
                                { joff = true })
      print("M5 x64 getmetatable behavior passed")
    end
  })

  add({
    name = "m5_x64_tget_array_header",
    description = "x64 TGET array bounds and FORWARD behavior",
    run = function(t)
      build_and_run_luajit_code(t, tget_array_header_smoke(), { joff = true })
      compile_and_run_c(t, t:tmp("lj_t-x64-tget-forward"),
                        "t-x64-tget-forward.c")
      print("M5 x64 TGET array bounds and FORWARD behavior passed")
    end
  })

  add({
    name = "m5_x64_tgets_node_order",
    description = "x64 TGETS/TSETS FORWARD behavior",
    run = function(t)
      build_and_run_luajit_code(t, tgets_node_order_smoke(), { joff = true })
      compile_and_run_c(t, t:tmp("lj_t-x64-tgets-forward"),
                        "t-x64-tgets-forward.c")
      print("M5 x64 TGETS/TSETS FORWARD behavior passed")
    end
  })

  add({
    name = "m5_x64_ipairs_snapshot",
    description = "x64 ipairs_aux array/hash FORWARD behavior",
    run = function(t)
      build_and_run_luajit_code(t, ipairs_snapshot_smoke(), { joff = true })
      compile_and_run_c(t, t:tmp("lj_t-x64-ipairs-forward"),
                        "t-x64-ipairs-forward.c")
      print("M5 x64 ipairs_aux array/hash FORWARD behavior passed")
    end
  })

  add({
    name = "m5_x64_itern_snapshot",
    description = "x64 BC_ITERN array/hash FORWARD behavior",
    run = function(t)
      build_and_run_luajit_code(t, itern_snapshot_smoke(), { joff = true })
      compile_and_run_c(t, t:tmp("lj_t-x64-itern-forward"),
                        "t-x64-itern-forward.c")
      print("M5 x64 BC_ITERN array/hash FORWARD behavior passed")
    end
  })

  add({
    name = "m5_x64_table_next_snapshot",
    description = "x64 lj_vm_next array/hash FORWARD behavior",
    run = function(t)
      build_and_run_luajit_code(t, table_next_snapshot_smoke())
      compile_and_run_c(t, t:tmp("lj_t-x64-vm-next-forward"),
                        "t-x64-vm-next-forward.c")
      print("M5 x64 lj_vm_next array/hash FORWARD behavior passed")
    end
  })

end
