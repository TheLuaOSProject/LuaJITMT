local th = require("threading")
local harness = require("thread_harness")

local function writer(tbl, ready, start, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  local roots = {}
  for i = 1, n do
    local key = { kind = "gc2-weakmeta", round = i }
    tbl[key] = { round = i }
    tbl["gc2-grow:" .. i] = i
    if i % 12 == 0 then roots[#roots + 1] = key end
    if #roots > 24 then roots[#roots - 23] = nil end
    if i % 31 == 0 then
      local probe = tbl.resize_meta_probe
      assert(type(probe) == "table" and probe.tag == "weak-meta-probe",
	     "weak metatable __index probe changed during GC2 bridge")
    end
    if i > 64 and i % 7 == 0 then
      tbl["gc2-grow:" .. (i - 32)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function collect_while_working(rounds)
  for i = 1, rounds do
    collectgarbage(i % 3 == 0 and "collect" or "step")
    if i % 8 == 0 then th.sleep(0.001) end
  end
end

local fallback = { resize_meta_probe = { tag = "weak-meta-probe" } }
local mt = { __mode = "k", __index = fallback }
local weak = setmetatable({}, mt)
local weak_mt = setmetatable({}, { __mode = "v" })
local weak_fallback = setmetatable({}, { __mode = "v" })

weak_mt[1] = mt
weak_fallback[1] = fallback
weak_fallback[2] = fallback.resize_meta_probe
fallback = nil
mt = nil

local ready = th.channel(1)
local start = th.channel(1)
local workers = { th.spawn(writer, weak, ready, start, 128) }
harness.wait_ready(ready, 1, 10, "gc2 weakmeta")
harness.release_start(start, 1, 10)
collect_while_working(128)
harness.join_all(workers, 30)
for _ = 1, 4 do collectgarbage("collect") end

assert(type(weak_mt[1]) == "table",
       "GC missed weak-key table metatable through GC2 bridge")
assert(type(weak_fallback[1]) == "table",
       "GC missed weak-key __index table through GC2 bridge")
assert(type(weak_fallback[2]) == "table",
       "GC missed weak-key __index probe through GC2 bridge")
assert(type(getmetatable(weak).__index) == "table")
assert(type(weak.resize_meta_probe) == "table")

print("t-gc2-weakmeta OK")
