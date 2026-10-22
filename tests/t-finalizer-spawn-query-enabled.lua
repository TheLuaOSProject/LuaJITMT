local ffi = require("ffi")
local th = require("threading")
ffi.cdef[[typedef struct { int x; } lj_gc_query_overlap_t;]]

local function setup_cdata(fin)
  ffi.gc(ffi.new("lj_gc_query_overlap_t"), fin)
end
local function setup_udata(fin)
  local u = newproxy(true)
  getmetatable(u).__gc = fin
end

local function run(label, setup, drive)
  local samples, release = th.channel(2), th.channel(1)
  local worker, during
  setup(function(_)
    worker = th.spawn(function(samples_ch, release_ch)
      assert(samples_ch:send(collectgarbage("isrunning") and "running" or "stopped", 2))
      local token, ok = release_ch:recv(10)
      assert(ok and token == "release")
      return collectgarbage("isrunning")
    end, samples, release)
    -- The child must publish its sample before this actual callback returns.
    local ok
    during, ok = samples:recv(2)
    assert(ok, label .. ": child did not sample inside callback")
  end)
  local completed = drive()
  local after = collectgarbage("isrunning")
  assert(worker and during, label .. ": callback did not execute")
  assert(completed ~= true, label .. ": explicit step completed with live child")
  assert(release:send("release", 2))
  local joined, child_after = worker:join(10)
  assert(joined, tostring(child_after))
  print(label .. " during=" .. tostring(during) .. " after=" .. tostring(after) .. " child_after=" .. tostring(child_after))
  assert(during == "running", label .. ": unexpected callback query")
  assert(after == true and child_after == true, label .. ": callback pause leaked")
  worker = nil
  collectgarbage("collect")
  collectgarbage("collect")
end

run("cdata full collect", setup_cdata, function() collectgarbage("collect") end)
run("cdata explicit step", setup_cdata, function() return collectgarbage("step", 1000000) end)
run("userdata full collect", setup_udata, function() collectgarbage("collect") end)
run("userdata explicit step", setup_udata, function() return collectgarbage("step", 1000000) end)
print("FINALIZER_SPAWN_QUERY_LOGICALLY_ENABLED passed")
