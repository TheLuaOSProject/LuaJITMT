local ffi = require("ffi")
local th = require("threading")

ffi.cdef[[
typedef struct { int x; } lj_m8_fin_spawn_live_t;
]]

local started = th.channel(1)
local release = th.channel(1)
local worker

local function make_finalizer()
  ffi.gc(ffi.new("lj_m8_fin_spawn_live_t"), function(_)
    worker = th.spawn(function(started_ch, release_ch)
      started_ch:send("started")
      local msg, ok = release_ch:recv(10)
      return ok == true and msg == "release"
    end, started, release)
  end)
end

make_finalizer()
collectgarbage("collect")

local msg, ok = started:recv(1)
assert(ok == true and msg == "started", "finalizer-spawned worker did not start")
assert(worker ~= nil, "finalizer did not publish worker handle")
assert(collectgarbage("isrunning") == true,
       "logical GC state was lost while finalizer-spawned worker was live")
assert(release:send("release", 1) == true)
local joined, result = worker:join(10)
assert(joined == true and result == true, tostring(result))
assert(collectgarbage("isrunning") == true,
       "logical GC state was lost after finalizer-spawned worker joined")

collectgarbage("collect")

print("t-m8-finalizer-spawn-live OK: finalizer-spawned worker can outlive callback")
