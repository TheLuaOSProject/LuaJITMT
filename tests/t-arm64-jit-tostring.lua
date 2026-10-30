-- Semantic companion for the bounded string-input tostring fast path.
local to = tostring

local function while_form(fn, value, n)
  local i, result = 0, nil
  while i < n do result = fn(value); i = i + 1 end
  return result
end

local function for_form(fn, value, n)
  local result
  for i = 1, n do result = fn(value) end
  return result
end

local strings = { "a\0b", "\128x", "" }
for _, value in ipairs(strings) do
  assert(while_form(to, value, 40) == value)
  assert(for_form(to, value, 40) == value)
end

assert(while_form(to, 2.75, 40) == "2.75")
assert(for_form(to, true, 40) == "true")
local calls = 0
local tab = setmetatable({}, { __tostring = function()
  calls = calls + 1
  return "table-tostring"
end })
assert(while_form(to, tab, 40) == "table-tostring" and calls == 40)
local bad = setmetatable({}, { __tostring = function()
  error("intentional tostring failure")
end })
local ok, err = pcall(while_form, to, bad, 40)
assert(not ok and tostring(err):find("intentional tostring failure", 1, true))
local replacement = function(v) return "replacement:" .. to(v) end
assert(for_form(replacement, strings[1], 40) == "replacement:" .. strings[1])
collectgarbage("collect")
assert(while_form(to, strings[2], 40) == strings[2])
