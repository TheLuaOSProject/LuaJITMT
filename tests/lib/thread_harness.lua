local M = {}

function M.arg_number(index, envname, default)
  local v = arg and arg[index] or nil
  if v == nil or v == "" then v = os.getenv(envname) end
  return tonumber(v) or default
end

function M.channels(nthreads)
  local th = require("threading")
  return th.channel(nthreads), th.channel(nthreads)
end

function M.wait_ready(ready, nthreads, timeout)
  timeout = timeout or 10
  for _ = 1, nthreads do
    local _, ok = ready:recv(timeout)
    assert(ok == true)
  end
end

function M.release_start(start, nthreads, timeout)
  timeout = timeout or 10
  for _ = 1, nthreads do
    assert(start:send("go", timeout) == true)
  end
end

function M.join_all(workers, timeout)
  timeout = timeout or 30
  for i = 1, #workers do
    local ok, result = workers[i]:join(timeout)
    assert(ok == true, tostring(result))
    assert(result == true)
  end
end

function M.fullgc(n)
  n = n or 2
  for _ = 1, n do
    collectgarbage("collect")
  end
end

return M
