local th = require"threading"
local jit = require"jit"

local ok, profile = pcall(require, "jit.profile")
if not ok then
  print("t-profile-blocked-tg SKIP: jit.profile unavailable")
  return
end

local ready = th.channel(0)
local release = th.channel(0)

local worker = th.spawn(function(ready_ch, release_ch)
  assert(ready_ch:send("ready", 10) == true)
  local token, ok_recv = release_ch:recv(10)
  assert(ok_recv == true and token == "release")
  return true
end, ready, release)

local token, ok_recv = ready:recv(10)
assert(ok_recv == true and token == "ready")
th.sleep(0.02)

local function collect(label, mode, enable_jit)
  if enable_jit then
    jit.on()
    jit.opt.start("hotloop=1")
  else
    jit.off()
  end

  local samples = 0
  local callbacks = 0
  local bad

  profile.start(mode, function(thread, n, vmstate)
    callbacks = callbacks + 1
    if type(thread) ~= "thread" or type(n) ~= "number" or
       type(vmstate) ~= "string" or #vmstate ~= 1 then
      bad = "bad jit.profile callback arguments"
      return
    end
    samples = samples + n
  end)

  local deadline = th.now() + 2
  local x = 0
  while samples == 0 and th.now() < deadline do
    for i = 1, 50000 do
      x = (x + i) % 1000003
    end
  end
  profile.stop()

  return {
    label = label,
    samples = samples,
    callbacks = callbacks,
    bad = bad,
    x = x
  }
end

local results = {
  collect("interp", "i1", false),
  collect("jit", "i1l", true)
}

assert(release:send("release", 1) == true)
local joined, result = worker:join(2)
assert(joined == true and result == true)

for _, r in ipairs(results) do
  assert(not r.bad, r.bad)
  assert(r.samples > 0,
         "jit.profile delivered no " .. r.label ..
         " samples while another TG was attached")
  assert(r.callbacks > 0 and r.x > 0)
end

print(("t-profile-blocked-tg OK: interp=%d/%d jit=%d/%d"):format(
  results[1].samples, results[1].callbacks,
  results[2].samples, results[2].callbacks))
