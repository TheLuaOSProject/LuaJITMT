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

function M.wait_ready(ready, nthreads, timeout, label)
  timeout = timeout or 10
  for _ = 1, nthreads do
    local _, ok = ready:recv(timeout)
    assert(ok == true, label and ("ready timeout: " .. label) or nil)
  end
end

function M.release_start(start, nthreads, timeout)
  timeout = timeout or 10
  for _ = 1, nthreads do
    assert(start:send("go", timeout) == true)
  end
end

function M.join_all(workers, timeout)
  M.join_each(workers, function(result)
    assert(result == true)
  end, timeout)
end

function M.join_count(workers, timeout)
  local total = 0
  M.join_each(workers, function(result)
    assert(type(result) == "number")
    total = total + result
  end, timeout)
  return total
end

function M.join_each(workers, check, timeout)
  timeout = timeout or 30
  for i = 1, #workers do
    local joined = { workers[i]:join(timeout) }
    local ok, result = joined[1], joined[2]
    assert(ok == true, tostring(result))
    if check then check(result, i, unpack(joined, 3)) end
  end
end

function M.fullgc(n)
  n = n or 2
  for _ = 1, n do
    collectgarbage("collect")
  end
end

return M
