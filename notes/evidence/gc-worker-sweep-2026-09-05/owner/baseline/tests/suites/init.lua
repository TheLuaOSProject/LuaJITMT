local function suite(name)
  local mod = require(name)
  return mod
end

local suites = {
  suite("m0"),
  suite("m2_arena"),
  suite("m3_gc"),
  suite("m4_threading"),
  suite("m5_fixtures"),
  suite("m5_aggregate"),
  suite("m5_publication"),
  suite("m5_tables"),
  suite("m5_runtime"),
  suite("m5_x64"),
  suite("m6_jit"),
  suite("m7_ffi"),
  suite("m8_weak"),
  suite("m9_m10_gc"),
  suite("release")
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
