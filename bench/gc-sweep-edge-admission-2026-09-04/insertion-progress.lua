local probe = assert(package.loadlib(arg[1], "luaopen_gcprobe"))()
local n = tonumber(arg[2]) or 10000
collectgarbage("collect")
probe(-1)
local t = {}
for i = 1, n do
  t["newk" .. i] = i
  if i % 250 == 0 then probe(i, t) end
end
probe(n + 1, t)
collectgarbage("collect")
probe(n + 2, t)
assert(t["newk" .. n] == n)
