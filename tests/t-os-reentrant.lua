local th = require"threading"
local harness = require"thread_harness"

local nthreads = harness.env_number("LJ_M5_OS_THREADS", 4)
local iters = harness.env_number("LJ_M5_OS_ITERS", 80)

assert(type(os.setlocale("C", "all")) == "string")

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

local function check_setlocale_blocked()
  local ok, err = pcall(os.setlocale, "C", "all")
  assert(ok == false)
  assert(tostring(err):find("os.setlocale mutation disabled", 1, true))
end

local function check_setlocale_query()
  local current = os.setlocale(nil, "all")
  assert(current == nil or type(current) == "string")
end

local workers = {}

for id = 1, nthreads do
  workers[id] = th.spawn(function(worker_id, n)
    check_setlocale_query()
    check_setlocale_blocked()
    for i = 1, n do
      check_date(worker_id, i)
      check_tmpname()
    end
    return worker_id, n
  end, id, iters)
end

harness.join_each(workers, function(worker_id, id, n)
  assert(worker_id == id)
  assert(n == iters)
end)

check_setlocale_query()
check_setlocale_blocked()

print(("t-os-reentrant OK: %d child TGs x %d os.date/tmpname ops"):format(
  nthreads, iters))
