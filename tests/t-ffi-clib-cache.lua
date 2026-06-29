local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CLIB_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CLIB_ITERS", 300)

ffi.cdef[[
int abs(int);
unsigned long strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, unsigned long);
int memcmp(const void *, const void *, unsigned long);
int atoi(const char *);
int toupper(int);
int tolower(int);
int getpid(void);
enum { LJ_M7_CLIB_CONST = 73 };
]]

collectgarbage("stop")

do
  local C = ffi.C
  local env = debug.getfenv(C)
  local abs = C.abs
  local strlen = C.strlen
  local const = C.LJ_M7_CLIB_CONST

  assert(env.abs == abs)
  assert(env.strlen == strlen)
  assert(const == 73)
  assert(env.LJ_M7_CLIB_CONST == 73)

  env.LJ_M7_CLIB_CONST = 731
  assert(C.LJ_M7_CLIB_CONST == 731)
  env.LJ_M7_CLIB_CONST = nil
  assert(C.LJ_M7_CLIB_CONST == 73)
  assert(env.LJ_M7_CLIB_CONST == 73)

  if jit and jit.status() then
    env.abs = function(x) return x + 1000 end
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    local function run(n)
      local sum = 0
      for i = 1, n do sum = sum + C.abs(i) end
      return sum
    end
    assert(run(80) == (80 * 1000) + ((80 * 81) / 2))
    jit.flush()
    env.abs = abs
  end

  do
    local oldenv = debug.getfenv(C)
    local newenv = {}
    debug.setfenv(C, newenv)
    assert(C.abs(-4) == 4)
    assert(newenv.abs == C.abs)
    debug.setfenv(C, oldenv)
  end
end

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local C = ffi.C
    require"thread_harness".assert_stack_grows(96)

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local seed = (id * 1000) + i
      assert(C.abs(-seed) == seed)
      assert(tonumber(C.strlen("abcdef")) == 6)
      assert(C.strcmp("cache", "cache") == 0)
      assert(C.strncmp("cache-a", "cache-b", 6) == 0)
      assert(C.memcmp("cache-a", "cache-b", 6) == 0)
      assert(C.atoi("12345") == 12345)
      assert(C.toupper(97) == 65)
      assert(C.tolower(65) == 97)
      assert(C.getpid() > 0)
      assert(C.LJ_M7_CLIB_CONST == 73)
    end

    return count
  end, ready, start, tid, iters)
  harness.wait_ready(ready, 1)
end

harness.release_start(start, nthreads)
local total = harness.join_count(workers)

collectgarbage("restart")
harness.fullgc()

assert(ffi.C.abs(-77) == 77)
assert(tonumber(ffi.C.strlen("cache-root")) == 10)
assert(ffi.C.LJ_M7_CLIB_CONST == 73)

local function exercise_load_unload(rounds)
  local ok = pcall(function()
    local cl = ffi.load("c")
    local env = debug.getfenv(cl)
    local abs = cl.abs
    assert(env.abs == abs)
    env.abs = function(x) return "override", x end
    local tag, v = cl.abs(-5)
    assert(tag == "override" and v == -5)
    env.abs = nil
    assert(cl.abs(-1) == 1)
    assert(env.abs == cl.abs)
    cl = nil
  end)
  if not ok then return 0 end
  for i = 1, rounds do
    local cl = ffi.load("c")
    assert(cl.abs(-i) == i)
    assert(tonumber(cl.strlen("retire")) == 6)
    cl = nil
    harness.fullgc(3)
  end
  return rounds
end

local unloads = exercise_load_unload(harness.env_number("LJ_M7_FFI_CLIB_UNLOADS", 16))

print(("t-ffi-clib-cache OK: %d threads, %d cache hits, %d unloads"):format(
  nthreads, total, unloads))
