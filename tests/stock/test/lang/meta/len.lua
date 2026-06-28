local mt = { __len = function(o, o2)
  assert(o2 == nil)
  return 42
end }

do --- table
  local t = {1,2,3}
  assert(#t == 3)
  assert(#"abcdef" == 6)

  setmetatable(t, { __foo = function() end })
  assert(#t == 3)
  assert(#t == 3)

  setmetatable(t, mt)
  assert(#t == 3) -- __len does NOT work on tables.
end

do --- userdata
  local u = newproxy(true)
  getmetatable(u).__len = function(o) return 42 end
  assert(#u == 42)
  local x = 0
  for i=1,100 do x = x + #u end
  assert(x == 4200)
end

do --- number
  debug.setmetatable(0, mt)
  assert(#1 == 42)
  debug.setmetatable(0, nil)
end
