local utils = require("suite_utils")
local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")

local build_and_run_luajit_code = runtime.build_and_run_luajit_code
local compile_and_run_c = build.compile_and_run_c

local function vm_case(vm, opname)
  local label = "case " .. opname .. ":"
  local first = vm:find(label, 1, true)
  if not first then error("x64 VM block not found: " .. label, 2) end
  local nextcase = vm:find("\n  case BC_", first + #label, true)
  if not nextcase then nextcase = #vm + 1 end
  return vm:sub(first, nextcase - 1)
end

local function assert_has(label, data, needle)
  checks.assert_text_contains(label, data, needle, "x64 VM text")
end

local function assert_missing(label, data, needle)
  if data:find(needle, 1, true) then
    error(label .. ": unexpected x64 VM text: " .. needle, 2)
  end
end

local function assert_count_at_least(label, data, needle, mincount)
  checks.assert_text_contains_count(label, data, needle, mincount,
                                    "x64 VM text")
end

local function assert_x64_vm_store_publication(t)
  local vm = utils.read_file(t:path("src", "vm_x64.dasc"))
  assert_missing("x64 VM store publication", vm, "barrierback")
  assert_missing("x64 VM store publication", vm, "lj_gc_barrieruv")

  local tsetv = vm_case(vm, "BC_TSETV")
  assert_has("BC_TSETV", tsetv, "call extern lj_tab_storetv_forvm_array")
  assert_has("BC_TSETV", tsetv, "->vm_gc2_barriertv_tab")

  local tsets = vm_case(vm, "BC_TSETS")
  assert_has("BC_TSETS", tsets, "jmp ->vmeta_tsets")
  assert_missing("BC_TSETS", tsets, "lj_tab_storetv")
  assert_missing("BC_TSETS", tsets, "vm_gc2_barriertv_tab")

  local tsetb = vm_case(vm, "BC_TSETB")
  assert_has("BC_TSETB", tsetb, "call extern lj_tab_storetv_forvm_array")
  assert_has("BC_TSETB", tsetb, "->vm_gc2_barriertv_tab")

  local tsetr = vm_case(vm, "BC_TSETR")
  assert_count_at_least("BC_TSETR", tsetr,
                        "call extern lj_tab_storetv_forvm_array", 2)
  assert_count_at_least("BC_TSETR", tsetr, "->vm_gc2_barriertv_tab", 2)

  local tsetm = vm_case(vm, "BC_TSETM")
  assert_has("BC_TSETM", tsetm,
             "call extern lj_tab_storetvn_forvm_array")

  local cset = vm_case(vm, "BC_CSET")
  assert_has("BC_CSET", cset, "call extern lj_func_storeuv_pub")

  local usetv = vm_case(vm, "BC_USETV")
  assert_has("BC_USETV", usetv, "call extern lj_func_storeuv_pub")

  local usets = vm_case(vm, "BC_USETS")
  assert_has("BC_USETS", usets, "call extern lj_func_storeuvstr_pub")

  local usetn = vm_case(vm, "BC_USETN")
  assert_has("BC_USETN", usetn, "call extern lj_func_storeuvnum_pub")

  local usetp = vm_case(vm, "BC_USETP")
  assert_has("BC_USETP", usetp, "call extern lj_func_storeuvpri_pub")
end

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
    name = "m5_x64_vm_store_publication",
    description = "x64 VM table/cell store publication surface",
    run = function(t)
      assert_x64_vm_store_publication(t)
      print("M5 x64 VM store publication surface passed")
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
