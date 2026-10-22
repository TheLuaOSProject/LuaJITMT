-- Returning below the trace entry must invalidate computed frame addresses.
if jit and jit.status() then
  jit.opt.start("hotloop=1", "hotexit=1", "recunroll=4")
end

local function inner(n)
  local value = n
  local function inc() value = value + 1 end
  inc()
  return value
end

local function retrec(n)
  if n == 0 then return 0 end
  local total = retrec(n - 1)
  local value = inner(n)
  return total + value
end

for i = 1, 120 do
  local result = retrec(12)
  assert(result == 90, tostring(result))
end

print("t-jit-cell-retf OK: captures after returns below trace entry")
