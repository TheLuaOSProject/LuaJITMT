local th = require"threading"

assert(type(th) == "table")
assert(type(th.cpucount) == "function")
assert(type(th.fence) == "function")
assert(type(th.sleep) == "function")
assert(type(th.spawn) == "function")
assert(type(th.current) == "function")
assert(type(th.mutex) == "function")
assert(type(th.channel) == "function")

local n = th.cpucount()
assert(type(n) == "number" and n >= 1)

assert(th.fence() == nil)
assert(th.sleep(0) == nil)

local ch = th.channel(2)
assert(type(ch) == "userdata")

local v, ok = ch:peek()
assert(v == nil and ok == false)

assert(ch:send("x") == true)
v, ok = ch:peek()
assert(v == "x" and ok == true)
assert(ch:send("y") == true)
local sent_timeout, sent_why = ch:send("full", 0)
assert(sent_timeout == nil and sent_why == "timeout")
v, ok = ch:recv()
assert(v == "x" and ok == true)
v, ok = ch:recv()
assert(v == "y" and ok == true)
v, ok = ch:recv(0)
assert(v == nil and ok == "timeout")

local delayed = th.channel(1)
local delayed_sender = th.spawn(function(q)
  th.sleep(0.01)
  q:send("late")
end, delayed)
v, ok = delayed:recv(1)
assert(v == "late" and ok == true)
assert(({ delayed_sender:join() })[1] == true)

local delayed_full = th.channel(1)
assert(delayed_full:send("first") == true)
local delayed_receiver = th.spawn(function(q)
  th.sleep(0.01)
  local rv, rok = q:recv()
  assert(rv == "first" and rok == true)
end, delayed_full)
sent_timeout, sent_why = delayed_full:send("second", 1)
assert(sent_timeout == true and sent_why == nil)
v, ok = delayed_full:recv()
assert(v == "second" and ok == true)
assert(({ delayed_receiver:join() })[1] == true)

local rendezvous = th.channel(0)
sent_timeout, sent_why = rendezvous:send("rv", 0)
assert(sent_timeout == nil and sent_why == "timeout")
v, ok = rendezvous:recv(0)
assert(v == nil and ok == "timeout")

local keepalive = th.channel(1)
local t = {marker = 42}
assert(keepalive:send(t) == true)
t = nil
collectgarbage()
collectgarbage()
v, ok = keepalive:recv()
assert(ok == true and v.marker == 42)

ch:close()
v, ok = ch:recv()
assert(v == nil and ok == false)
local sent, err = pcall(function() ch:send("y") end)
assert(sent == false and tostring(err):match("closed channel"))

local made, rangeerr = pcall(function() th.channel(-1) end)
assert(made == false and tostring(rangeerr):match("out of range"))

local me = th.current()
assert(type(me) == "userdata")
assert(type(me:id()) == "number")
assert(me:running() == true)

local worker = th.spawn(function(a, b) return a + b, nil, "x" end, 40, 2)
assert(type(worker) == "userdata")
assert(type(worker:id()) == "number")
assert(worker:id() ~= me:id())
local wok, sum, hole, tag = worker:join()
assert(wok == true and sum == 42 and hole == nil and tag == "x")
assert(worker:running() == false)

local idem = th.spawn(function() return 7 end)
local a = { idem:join() }
local b = { idem:join() }
assert(a[1] == true and a[2] == 7 and b[1] == true and b[2] == 7)

local errthr = th.spawn(function() error("boom", 0) end)
local eok, err = errthr:join()
assert(eok == false and err == "boom")

local slow = th.spawn(function()
  th.sleep(0.1)
  return "done"
end)
local timed, why = slow:join(0)
assert(timed == nil and why == "timeout")
local slowres = { slow:join() }
assert(slowres[1] == true and slowres[2] == "done")

local self = th.spawn(function() return th.current():id() end)
local sok, cid = self:join()
assert(sok == true and cid == self:id() and cid ~= me:id())

local target = th.spawn(function()
  th.sleep(0.05)
  return "ok"
end)
local waiter = th.spawn(function(x) return x:join() end, target)
local main_join = { target:join() }
local waiter_join = { waiter:join() }
assert(main_join[1] == true and main_join[2] == "ok")
assert(waiter_join[1] == true and waiter_join[2] == true and waiter_join[3] == "ok")

local m = th.mutex()
assert(type(m) == "userdata")
assert(m:trylock() == true)
assert(m:trylock() == false)
assert(m:unlock() == nil)
assert(m:lock() == nil)
local locked = th.channel(1)
local lock_worker = th.spawn(function(mm, out)
  mm:lock()
  out:send("locked")
  mm:unlock()
end, m, locked)
local pv, pok = locked:peek()
assert(pv == nil and pok == false)
m:unlock()
local locked_value, locked_ok = locked:recv()
assert(locked_value == "locked" and locked_ok == true)
assert(({ lock_worker:join() })[1] == true)
local unlock_ok, unlock_err = pcall(function() m:unlock() end)
assert(unlock_ok == false and tostring(unlock_err):match("unlock of unlocked mutex"))

local th2 = require"threading"
assert(th2 == th)

print("t-threading-api OK: cpucount, fence, sleep, channel, spawn, mutex registered")
