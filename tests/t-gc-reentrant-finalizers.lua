local ffi = require("ffi")

local function check(outer, inner, kind)
  local calls, depth, nested, returned = 0, 0, 0, false
  local kept = {}
  local function finish(obj, id)
    depth, calls = depth + 1, calls + 1
    kept[id] = obj
    if depth == 1 and calls == 1 then
      if inner == "collect" then
        collectgarbage()
      else
        for i = 1, 256 do
          if nested ~= 0 then break end
          collectgarbage("step", 1)
        end
      end
      returned = nested == 1 and depth == 1 and kept[id] == obj
    elseif depth == 2 then
      nested = nested + 1
    end
    depth = depth - 1
  end
  local function make(id)
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
  assert(calls == 2 and nested == 1 and returned and depth == 0,
         outer .. "/" .. inner .. "/" .. kind)
  for id = 1, 2 do
    if type(kept[id]) == "cdata" then
      assert(kept[id][0] == id)
    else
      assert(type(kept[id]) == "userdata")
    end
  end
  kept = {}
  collectgarbage()
  assert(calls == 2)
end

for _, outer in ipairs({"step", "collect"}) do
  for _, inner in ipairs({"step", "collect"}) do
    for _, kind in ipairs({"userdata", "cdata", "mixed"}) do
      check(outer, inner, kind)
    end
  end
end
print("t-gc-reentrant-finalizers OK")
