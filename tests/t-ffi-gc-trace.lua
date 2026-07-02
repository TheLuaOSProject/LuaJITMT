local ffi = require"ffi"
local trace_count = require"jit_harness".trace_count

ffi.cdef[[
typedef struct { int x; } lj_m7_fin_trace_t;
typedef struct { int x; } lj_m7_fin_trace_mt_t;
]]

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")

local trace_t = ffi.typeof("lj_m7_fin_trace_t")
local direct_finalized = 0
local cleared_finalized = 0
local mt_finalized = 0

local function direct_fin(_)
  direct_finalized = direct_finalized + 1
end

local function cleared_fin(_)
  cleared_finalized = cleared_finalized + 1
end

local mt_t = ffi.metatype("lj_m7_fin_trace_mt_t", {
  __gc = function(_)
    mt_finalized = mt_finalized + 1
  end
})

local function direct_loop(n)
  for i = 1, n do
    ffi.gc(trace_t(i), direct_fin)
  end
end

local function clear_loop(n)
  for i = 1, n do
    local cd = ffi.gc(trace_t(i), cleared_fin)
    ffi.gc(cd, nil)
  end
end

local function metatype_loop(n)
  for i = 1, n do
    mt_t(i)
  end
end

collectgarbage("stop")

local rounds = 20
local n = 40
for _ = 1, rounds do direct_loop(n) end
for _ = 1, rounds do clear_loop(n) end
for _ = 1, rounds do metatype_loop(n) end

local traces = trace_count(64)
assert(traces >= 1, "expected metatype __gc recorder trace")

collectgarbage("restart")
collectgarbage("collect")
collectgarbage("collect")

local expected = rounds * n
assert(direct_finalized == expected,
       ("direct ffi.gc finalized %d, expected %d"):
       format(direct_finalized, expected))
assert(cleared_finalized == 0,
       ("ffi.gc clear finalized %d, expected 0"):
       format(cleared_finalized))
assert(mt_finalized == expected,
       ("traced ctype __gc finalized %d, expected %d"):
       format(mt_finalized, expected))

print(("t-ffi-gc-trace OK: %d metatype traces, %d direct finalizers, %d metatype finalizers"):
      format(traces, direct_finalized, mt_finalized))
