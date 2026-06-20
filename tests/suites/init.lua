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
  assert(tests[test.name] == nil, "duplicate test: " .. test.name)
  tests[test.name] = test
end

local function validate_deps(test)
  local deps = test.deps
  if deps == nil then return end
  assert(type(deps) == "table", test.name .. " deps must be a table")
  local seen = {}
  for i = 1, #deps do
    local dep = deps[i]
    assert(type(dep) == "string" and dep ~= "",
           test.name .. " dependency " .. i .. " must be a name")
    assert(dep ~= test.name, test.name .. " cannot depend on itself")
    assert(not seen[dep], test.name .. " has duplicate dependency: " .. dep)
    seen[dep] = true
    assert(tests[dep] ~= nil,
           test.name .. " depends on unknown test: " .. dep)
  end
end

for i = 1, #suites do
  suites[i](add)
end

for _, test in pairs(tests) do
  validate_deps(test)
end

return tests
