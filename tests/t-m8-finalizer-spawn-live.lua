local ffi = require("ffi")
local th = require("threading")

ffi.cdef[[
typedef struct { int x; } lj_m8_fin_spawn_live_t;
]]

local function setup_cdata_finalizer(fin)
  ffi.gc(ffi.new("lj_m8_fin_spawn_live_t"), fin)
end

local function setup_udata_finalizer(fin)
  local u = newproxy(true)
  getmetatable(u).__gc = fin
end

local function run_spawn_live(label, setup_finalizer, drive)
  local started = th.channel(1)
  local release = th.channel(1)
  local worker

  setup_finalizer(function(_)
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

run_spawn_live("cdata full collect", setup_cdata_finalizer, function()
  collectgarbage("collect")
end)

run_spawn_live("cdata explicit step", setup_cdata_finalizer, function()
  return collectgarbage("step", 1000000)
end)

run_spawn_live("userdata full collect", setup_udata_finalizer, function()
  collectgarbage("collect")
end)

run_spawn_live("userdata explicit step", setup_udata_finalizer, function()
  return collectgarbage("step", 1000000)
end)

print("t-m8-finalizer-spawn-live OK: cdata/userdata finalizer-spawned worker can outlive callback")
