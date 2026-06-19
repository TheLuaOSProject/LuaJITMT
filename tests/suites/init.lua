local suites = {
  require("m2_arena"),
  require("m4_threading"),
  require("m5_tables"),
  require("m5_runtime")
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
