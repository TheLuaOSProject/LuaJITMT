-- Fresh-process diagnostic benchmark. GC remains enabled throughout.
local n = tonumber(arg[1]) or 5000
assert(n >= 1 and n % 1 == 0)
assert(collectgarbage("isrunning"))
collectgarbage("collect")
local t = {}
local started = os.clock()
for i = 1, n do
  t["newk" .. i] = i
end
local elapsed = os.clock() - started
collectgarbage("collect")
local sum, count = 0, 0
for i = 1, n do
  local value = t["newk" .. i]
  assert(value == i)
  sum = sum + value
end
for _ in pairs(t) do count = count + 1 end
assert(count == n and sum == n * (n + 1) / 2)
assert(collectgarbage("isrunning"))
print(string.format("%d,%.9f,%.3f,%.0f,%d", n, elapsed,
                    elapsed / n * 1e9, sum, count))
