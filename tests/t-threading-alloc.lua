local th = require"threading"

local nthread = tonumber(arg and arg[1]) or 4
local niter = tonumber(arg and arg[2]) or 6000
local threads = {}

for id = 1, nthread do
  threads[id] = th.spawn(function(id, niter)
    local total = 0
    for i = 1, niter do
      local shared = "m5-alloc-shared-" .. (i % 256)
      local unique = "m5-alloc-" .. id .. "-" .. (i % 512)
      local grown = unique .. ":" .. shared .. ":" .. string.rep("x", i % 31)
      total = total + #grown
      if #shared == 0 or #grown == 0 then
	error("bad allocation result")
      end
    end
    return total
  end, id, niter)
end

for id = 1, nthread do
  local ok, total = threads[id]:join()
  assert(ok == true)
  assert(total > niter)
end

collectgarbage("collect")
collectgarbage("collect")

print(("t-threading-alloc OK: %d workers x %d concat/intern ops"):format(
  nthread, niter))
