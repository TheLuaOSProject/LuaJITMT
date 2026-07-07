local th = require("threading")

local function exercise(expect_seen)
  local seen = 0
  local function hook(ev)
    assert(ev == "flush", ev)
    seen = seen + 1
    local s = 0
    for i = 1, 8 do s = s + i end
    assert(s == 36)
  end
  jit.attach(hook, "trace")
  jit.flush()
  jit.attach(hook)
  assert(seen == expect_seen, seen)
end

exercise(1)

local worker = th.spawn(function()
  exercise(0)
  return true
end)

assert(worker:join(20) == true)
print("t-jit-vmevent-flush OK: trace event flush hooks keep TG dispatch")
