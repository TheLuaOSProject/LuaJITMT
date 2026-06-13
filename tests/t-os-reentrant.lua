local th = require"threading"

local nthreads = tonumber(os.getenv("LJ_M5_OS_THREADS") or "4")
local iters = tonumber(os.getenv("LJ_M5_OS_ITERS") or "80")

local function check_date(id, i)
  local t = 1609459200 + id * 1000 + i
  local utc = os.date("!*t", t)
  assert(type(utc) == "table")
  assert(utc.year >= 2021)
  assert(type(utc.sec) == "number")
  local s = os.date("!%Y-%m-%d %H:%M:%S", t)
  assert(type(s) == "string" and #s == 19)
  local localt = os.date("*t", t)
  assert(type(localt) == "table")
  assert(localt.isdst == nil or type(localt.isdst) == "boolean")
end

local function check_tmpname()
  local name = os.tmpname()
  assert(type(name) == "string" and #name > 0)
  local ok, err = os.remove(name)
  assert(ok, err)
end

local workers = {}

for id = 1, nthreads do
  workers[id] = th.spawn(function(worker_id, n)
    for i = 1, n do
      check_date(worker_id, i)
      check_tmpname()
    end
    return worker_id, n
  end, id, iters)
end

for id = 1, nthreads do
  local worker = workers[id]
  local ok, worker_id, n = worker:join()
  assert(ok == true)
  assert(worker_id == id)
  assert(n == iters)
end

print(("t-os-reentrant OK: %d child TGs x %d os.date/tmpname ops"):format(
  nthreads, iters))
