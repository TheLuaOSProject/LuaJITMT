local ffi = require("ffi")

local function check(outer, inner, kind, fresh)
  local calls, depth, nested, returned = 0, 0, 0, false
  local kept = {}
  local make
  local count = fresh and 3 or 2
  local function finish(obj, id)
    depth, calls = depth + 1, calls + 1
    kept[id] = obj
    if depth == 1 and calls == 1 then
      if fresh then make(3) end
      if inner == "collect" then
        collectgarbage()
      else
        for i = 1, 256 do
          if nested ~= 0 then break end
          collectgarbage("step", 1)
        end
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
  make(1)
  make(2)
  collectgarbage("restart")
  if outer == "collect" then
    collectgarbage()
  else
    for i = 1, 4096 do
      if calls >= 2 then break end
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
      check(outer, inner, kind, false)
      if inner == "collect" then check(outer, inner, kind, true) end
    end
  end
end
print("t-gc-reentrant-finalizers OK")
