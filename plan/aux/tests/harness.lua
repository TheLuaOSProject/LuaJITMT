-- harness.lua — micro test runner for the LuaJIT-MT suite (13 §13.1).
-- Each t-*.lua does: local T = require("harness"); T.case(name, fn); T.done()
local M = { n = 0, failed = 0 }

function M.case(name, fn)
  M.n = M.n + 1
  local ok, err = xpcall(fn, debug.traceback)
  if ok then
    io.write("ok    ", name, "\n")
  else
    M.failed = M.failed + 1
    io.write("FAIL  ", name, "\n", tostring(err), "\n")
  end
end

function M.eq(a, b, msg)
  if a ~= b then
    error(("%s: expected %s, got %s"):format(msg or "eq", tostring(b), tostring(a)), 2)
  end
end

function M.truthy(v, msg) if not v then error(msg or "expected truthy", 2) end end

-- repeat a racy scenario many times; any single failure fails the case
function M.reps(n, fn) for i = 1, n do fn(i) end end

function M.done()
  io.write(("-"):rep(40), "\n")
  io.write(("%d cases, %d failed\n"):format(M.n, M.failed))
  os.exit(M.failed == 0 and 0 or 1)
end

return M
