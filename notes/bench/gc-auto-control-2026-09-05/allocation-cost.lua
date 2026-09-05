local kind, n = assert(arg[1]), tonumber(arg[2]) or 100000
local ring = {}
for i = 1, 128 do ring[i] = {} end
local runs = {
  tnew = function(count)
    local sum = 0
    for i = 1, count do
      local t = {}
      t.value = i
      ring[i % 128 + 1] = t
      sum = sum + t.value
    end
    return sum
  end,
  tdup = function(count)
    local sum = 0
    for i = 1, count do
      local t = { value = 0 }
      t.value = i
      ring[i % 128 + 1] = t
      sum = sum + t.value
    end
    return sum
  end,
}
local function main()
  local run = assert(runs[kind])
  collectgarbage("collect")
  collectgarbage("collect")
  assert(collectgarbage("isrunning"))
  jit.opt.start("hotloop=1")
  assert(run(1000) == 500500)
  local best = math.huge
  for pass = 1, 5 do
    collectgarbage("collect")
    assert(collectgarbage("isrunning"))
    local start = os.clock()
    local sum = run(n)
    local seconds = os.clock() - start
    assert(sum == n * (n + 1) / 2)
    assert(ring[n % 128 + 1].value == n)
    best = math.min(best, seconds)
    print("sample", pass, string.format("%.9f", seconds))
  end
  print("result", kind, n, string.format("%.9f", best))
end
jit.off(main)
main()
