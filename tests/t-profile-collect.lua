local jit = require("jit")
local ok, profile = pcall(require, "jit.profile")
if not ok then
  print("t-profile-collect SKIP: jit.profile unavailable")
  return
end

jit.off()
jit.flush()

local weak = setmetatable({}, { __mode = "v" })
local live = {}
weak[2] = live
local called, cleared, survived, sampled_main

local function make_weak_value()
  weak[1] = {}
end

profile.start("i2", function(sampled, samples)
  if called then return end
  make_weak_value()
  collectgarbage("collect")
  cleared = weak[1] == nil
  survived = weak[2] ~= nil
  sampled_main = type(sampled) == "thread" and
                 sampled ~= coroutine.running() and samples > 0
  called = true
end)

local deadline = os.clock() + 3
while not called and os.clock() < deadline do end
profile.stop()

assert(called and sampled_main, "profiler did not sample the main thread")
assert(cleared, "collection in the profiler callback retained the weak value")
assert(survived and weak[2] == live, "collection lost a live main-thread root")

print("t-profile-collect OK")
