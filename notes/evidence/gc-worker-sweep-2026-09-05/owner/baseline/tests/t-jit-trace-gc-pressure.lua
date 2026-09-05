local threading = require("threading")

if not jit or not jit.status or not jit.status() then
  return
end

local function compile_throwaway(n)
  for _ = 1, n do
    assert(loadstring("for i = 1, 100 do end"))()
  end
end

-- A bounded trace-publication burst must request ordinary GC2 progress even
-- when the compatibility allocation trigger is inflated by fixed runtime
-- state. The request is asynchronous; trace liveness remains GC2's decision.
jit.flush()
collectgarbage()
local before = threading.gcstats().cycle_requests
compile_throwaway(80)
local after = threading.gcstats().cycle_requests
assert(after > before, "trace-slot pressure did not request a GC2 cycle")

-- Pressure is an automatic-GC source and must honor the public stop gate.
jit.flush()
collectgarbage()
collectgarbage("stop")
before = threading.gcstats().cycle_requests
compile_throwaway(80)
after = threading.gcstats().cycle_requests
assert(after == before, "trace-slot pressure bypassed collectgarbage('stop')")
collectgarbage("restart")
collectgarbage()

print("JIT trace-slot GC2 pressure behavior passed")
