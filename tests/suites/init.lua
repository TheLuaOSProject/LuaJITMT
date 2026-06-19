local suites = {
  require("m0"),
  require("m2_arena"),
  require("m3_gc"),
  require("m4_threading"),
  require("m5_fixtures"),
  require("m5_aggregate"),
  require("m5_publication"),
  require("m5_tables"),
  require("m5_runtime"),
  require("m5_x64"),
  require("m6_jit"),
  require("m7_ffi"),
  require("m8_weak"),
  require("m9_m10_gc")
}

local tests = {}

local function add(test)
  assert(type(test.name) == "string" and test.name ~= "", "test needs a name")
  assert(type(test.run) == "function", test.name .. " needs a run function")
  tests[test.name] = test
end

for i = 1, #suites do
  suites[i](add)
end

return tests
