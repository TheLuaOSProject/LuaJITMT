local function iterator()
  local values = {}
  for k, v in ipairs({11,12,13,14,15,16,17,18}) do
    values[k] = function() return k, v end
  end
  return values
end

local function repeated()
  local values = {}
  local i = 0
  repeat
    i = i + 1
    local x = i
    values[i] = function(n) x = x + (n or 0); return x end
  until i == 8
  return values
end

local function branch(x, take)
  if take then
    local function get() return x end
    x = x + 1
    return get()
  end
  return x
end

return iterator, repeated, branch
