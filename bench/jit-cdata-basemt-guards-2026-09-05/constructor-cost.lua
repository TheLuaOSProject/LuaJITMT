local ffi = require("ffi")
local util = require("jit.util")
local mode = assert(arg[1])
local n = tonumber(arg[2]) or (mode == "nosink" and 300000 or 3000000)
local ct = ffi.typeof("struct { double x; double y; }")
local function run(count)
  local sum = 0
  for i = 1, count do
    local obj = ct(i, i + 1)
    sum = sum + obj.x + obj.y
  end
  return sum
end
local function tracecount()
  local count = 0
  for i = 1, 100 do if util.traceinfo(i) then count = count + 1 end end
  return count
end
local exits = 0
local function onexit() exits = exits + 1 end
jit.off(tracecount, true)
jit.off(onexit, true)
local function main()
  assert(jit.status(), "cost comparison requires native JIT")
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1000")
  if mode == "nosink" then jit.opt.start("-sink") else assert(mode == "sink") end
  jit.attach(onexit, "texit")
  assert(run(80) == 80 * 82)
  assert(tracecount() > 0 and exits > 0, "warm loop must execute native code")
  jit.attach(onexit)
  print("constructor", mode, "iterations", n, "warm-native-exits", exits)
  for rep = 1, 5 do
    collectgarbage("collect")
    local start = os.clock()
    local sum = run(n)
    local seconds = os.clock() - start
    assert(sum == n * (n + 2), "constructor sum")
    print(string.format("sample %d %.9f %.9f", rep, seconds, seconds / n * 1e9))
  end
end
jit.off(main, true)
main()
