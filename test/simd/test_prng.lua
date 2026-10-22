-- A CCI_CASTU64 result is a full double bit pattern returned in a GPR.
-- Losing REX.W on the SSE-only MOVQ path used to truncate it to 32 bits,
-- making compiled math.random() return -1 as soon as the loop got hot.
local T = require("simdtest")
local jit = require("jit")
local N = 512

local function reference(mode)
  local out = {}
  for i = 1, N do
    if mode == 0 then out[i] = math.random()
    elseif mode == 1 then out[i] = math.random(10)
    else out[i] = math.random(-3, 11) end
  end
  return out
end
jit.off(reference, true)

local function recorded(mode)
  local out = {}
  for i = 1, N do
    if mode == 0 then out[i] = math.random()
    elseif mode == 1 then out[i] = math.random(10)
    else out[i] = math.random(-3, 11) end
  end
  return out
end

for mode = 0, 2 do
  T.test("PRNG full-width call result "..mode, function()
    math.randomseed(17)
    local expected = reference(mode)
    math.randomseed(17)
    local actual = recorded(mode)
    for i = 1, N do
      T.checkeq(actual[i], expected[i], "PRNG overload "..mode.." sample "..i)
    end
  end)
end

return T
