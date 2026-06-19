local root = os.getenv("LJ_TEST_ROOT") or "."
package.path = root .. "/tests/lib/?.lua;" ..
               root .. "/tests/suites/?.lua;" ..
               package.path

local ljtest = require("ljtest")
local tests = require("init")

local function sorted_names()
  local names = {}
  for name in pairs(tests) do names[#names + 1] = name end
  table.sort(names)
  return names
end

local function usage()
  io.stderr:write("usage: lua tools/test.lua [--list] <test> [<test> ...] [-- <args> ...]\n")
  io.stderr:write("available tests:\n")
  local names = sorted_names()
  for i = 1, #names do
    local test = tests[names[i]]
    io.stderr:write("  " .. names[i])
    if test.description then io.stderr:write(" - " .. test.description) end
    io.stderr:write("\n")
  end
end

if arg[1] == "--list" then
  local names = sorted_names()
  for i = 1, #names do print(names[i]) end
  os.exit(0)
end

if #arg == 0 then
  usage()
  os.exit(2)
end

local names = {}
local passthrough = {}
local in_passthrough = false
for i = 1, #arg do
  if in_passthrough then
    passthrough[#passthrough + 1] = arg[i]
  elseif arg[i] == "--" then
    in_passthrough = true
  else
    names[#names + 1] = arg[i]
  end
end

if #names == 0 then
  usage()
  os.exit(2)
end

if #passthrough > 0 and #names ~= 1 then
  io.stderr:write("passthrough args require exactly one test\n")
  os.exit(2)
end

local t = ljtest.new(root)
for i = 1, #names do
  local name = names[i]
  local test = tests[name]
  if not test then
    io.stderr:write("unknown test: " .. name .. "\n")
    usage()
    os.exit(2)
  end
  io.stderr:write("== " .. name .. " ==\n")
  test.run(t, passthrough)
  io.stderr:write("ok " .. name .. "\n")
end
