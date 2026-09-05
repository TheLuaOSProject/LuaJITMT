local clock = os.clock
local count = tonumber(arg[1]) or 100000
local mode = arg[2] or "field"
local t = { offset = 17, flag = true, [3] = 17 }
local run
if mode == "field" then
  run = function()
    local sum = 0
    for i = 1, count do sum = sum + t.offset end
    return sum
  end
elseif mode == "array" then
  run = function()
    local sum = 0
    for i = 1, count do sum = sum + t[3] end
    return sum
  end
elseif mode == "boolean" then
  run = function()
    local sum = 0
    for i = 1, count do if t.flag then sum = sum + 1 end end
    return sum
  end
else
  error("unknown mode")
end
local started = clock()
local result = run()
local elapsed = clock() - started
assert(result == count * (mode == "boolean" and 1 or 17))
collectgarbage("collect")
print(string.format("%s %d %.9f", mode, result, elapsed))
