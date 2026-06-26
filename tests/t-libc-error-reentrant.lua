local th = require"threading"
local harness = require"thread_harness"

local nthreads = harness.env_number("LJ_M5_ERRNO_THREADS", 6)
local iters = harness.env_number("LJ_M5_ERRNO_ITERS", 160)

local function missing_name(worker, i)
  return "/tmp/lj-lockless-missing-" .. worker .. "-" .. i .. "-" ..
         tostring({})
end

local function expect_missing_io(name)
  local f, err, code = io.open(name, "rb")
  assert(f == nil)
  assert(type(err) == "string" and err:find(name, 1, true))
  assert(type(code) == "number")
end

local function expect_missing_load(name)
  local fn, err = loadfile(name)
  assert(fn == nil)
  assert(type(err) == "string" and err:find(name, 1, true))
end

local function expect_missing_remove(name)
  local ok, err, code = os.remove(name)
  assert(ok == nil)
  assert(type(err) == "string" and err:find(name, 1, true))
  assert(type(code) == "number")
end

local workers = {}
for worker = 1, nthreads do
  workers[worker] = th.spawn(function(id, n)
    for i = 1, n do
      local name = missing_name(id, i)
      expect_missing_io(name)
      expect_missing_load(name)
      expect_missing_remove(name)
    end
    return n
  end, worker, iters)
end

harness.join_each(workers, function(done)
  assert(done == iters)
end)

print(("t-libc-error-reentrant OK: %d child TGs x %d errno paths"):format(
  nthreads, iters))
