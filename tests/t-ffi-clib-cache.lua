local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_CLIB_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_CLIB_ITERS")) or 300

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

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local C = ffi.C

    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

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
  local _, ok = ready:recv(10)
  assert(ok == true)
end

for _ = 1, nthreads do
  assert(start:send("go", 10) == true)
end

local total = 0
for tid = 1, nthreads do
  local ok, result = workers[tid]:join(30)
  assert(ok == true, tostring(result))
  assert(type(result) == "number")
  total = total + result
  workers[tid] = nil
end

collectgarbage("restart")
collectgarbage("collect")
collectgarbage("collect")

assert(ffi.C.abs(-77) == 77)
assert(tonumber(ffi.C.strlen("cache-root")) == 10)
assert(ffi.C.LJ_M7_CLIB_CONST == 73)

print(("t-ffi-clib-cache OK: %d threads, %d cache hits"):format(nthreads, total))
