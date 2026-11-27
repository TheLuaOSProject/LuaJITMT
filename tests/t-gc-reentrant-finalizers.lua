local ffi = require("ffi")

local function check(outer, inner, kind, fresh, initial, budget)
  local calls, depth, nested, returned = 0, 0, 0, false
  local kept = {}
  local make
  local count = initial + (fresh and 1 or 0)
  local function finish(obj, id)
    depth, calls = depth + 1, calls + 1
    kept[id] = obj
    if depth == 1 and calls == 1 then
      if fresh then make(count) end
      if inner == "collect" then
        collectgarbage()
      else
        local complete = false
        for i = 1, 256 do
          if collectgarbage("step", budget) then complete = true end
          if nested == count - 1 and (complete or not fresh) then break end
        end
        if fresh then assert(complete) end
      end
      returned = nested == count - 1 and depth == 1 and kept[id] == obj
    elseif depth == 2 then
      nested = nested + 1
    end
    depth = depth - 1
  end
  make = function(id)
    local fin = function(obj) finish(obj, id) end
    if kind == "cdata" or (kind == "mixed" and id == 2) then
      ffi.gc(ffi.new("int[1]", id), fin)
    else
      local obj = newproxy(true)
      getmetatable(obj).__gc = fin
    end
  end

  collectgarbage()
  collectgarbage("stop")
  for id = 1, initial do make(id) end
  collectgarbage("restart")
  if outer == "collect" then
    collectgarbage()
  else
    for i = 1, 4096 do
      if calls >= count then break end
      collectgarbage("step", 1)
    end
  end
  assert(calls == count and nested == count - 1 and returned and depth == 0,
         outer .. "/" .. inner .. "/" .. kind .. (fresh and "/fresh" or ""))
  for id = 1, count do
    if type(kept[id]) == "cdata" then
      assert(kept[id][0] == id)
    else
      assert(type(kept[id]) == "userdata")
    end
  end
  kept = {}
  collectgarbage()
  assert(calls == count)
end

for _, outer in ipairs({"step", "collect"}) do
  for _, inner in ipairs({"step", "collect"}) do
    for _, kind in ipairs({"userdata", "cdata", "mixed"}) do
      check(outer, inner, kind, false, 2, 1)
      check(outer, inner, kind, true, 2, 1)
      if inner == "step" then check(outer, inner, kind, true, 1, 64) end
    end
  end
end
print("t-gc-reentrant-finalizers OK")
