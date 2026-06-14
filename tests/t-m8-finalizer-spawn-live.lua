local ffi = require("ffi")
local th = require("threading")

ffi.cdef[[
typedef struct { int x; } lj_m8_fin_spawn_live_t;
]]

local function run_spawn_live(label, drive)
  local started = th.channel(1)
  local release = th.channel(1)
  local worker

  ffi.gc(ffi.new("lj_m8_fin_spawn_live_t"), function(_)
    worker = th.spawn(function(started_ch, release_ch)
      started_ch:send("started")
      local msg, ok = release_ch:recv(10)
      return ok == true and msg == "release"
    end, started, release)
  end)

  local complete = drive()

  local msg, ok = started:recv(1)
  assert(ok == true and msg == "started",
         label .. ": finalizer-spawned worker did not start")
  assert(worker ~= nil, label .. ": finalizer did not publish worker handle")
  assert(complete ~= true,
         label .. ": GC step completed while finalizer-spawned worker was live")
  assert(collectgarbage("isrunning") == true,
         label .. ": logical GC state was lost while worker was live")
  assert(release:send("release", 1) == true)
  local joined, result = worker:join(10)
  assert(joined == true and result == true, tostring(result))
  worker = nil
  assert(collectgarbage("isrunning") == true,
         label .. ": logical GC state was lost after worker joined")

  collectgarbage("collect")
  collectgarbage("collect")
end

run_spawn_live("full collect", function()
  collectgarbage("collect")
end)

run_spawn_live("explicit step", function()
  return collectgarbage("step", 1000000)
end)

print("t-m8-finalizer-spawn-live OK: finalizer-spawned worker can outlive callback")
