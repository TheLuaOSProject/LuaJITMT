local M = {}

local util = require("jit.util")

function M.trace_count(limit)
  local n = 0
  limit = limit or 64
  for i = 1, limit do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end

jit.off(M.trace_count, true)

return M
