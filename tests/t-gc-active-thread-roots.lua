local th = require"threading"
local harness = require"thread_harness"

local function assert_stdlib_roots()
  assert(type(print) == "function")
  assert(type(collectgarbage) == "function")
  assert(type(io) == "table")
  assert(io.stdout ~= nil)
  assert(type(string.format) == "function")
end

local function churn_closures(n)
  local s = 0
  for i = 1, n do
    local x = i
    local f = function()
      x = x + 1
      return x
    end
    s = s + f()
  end
  return s
end

local function native_leave_gc_preserves_caller_frame()
  local ready = th.channel(1)
  local requests = th.channel(1)
  local replies = th.channel(1)
  local sleep = th.sleep
  local worker = th.spawn(function(ready_ch, request_ch, reply_ch)
    assert(ready_ch:send("ready", 10) == true)
    while true do
      local token, ok = request_ch:recv(10)
      assert(ok == true, "native GC worker request timeout")
      if token == "stop" then break end
      for i = 1, 3 do
	local garbage = {}
	for j = 1, 64 do
	  garbage[j] = { token, i, j, "native-leave-gc" }
	end
	collectgarbage(i == 3 and "collect" or "step")
      end
      assert(reply_ch:send(token, 10) == true)
    end
    return true
  end, ready, requests, replies)

  do
    local token, ok = ready:recv(10)
    assert(ok == true and token == "ready")
  end

  for round = 1, 24 do
    local k01 = { round, 1 }
    local k02 = { round, 2 }
    local k03 = { round, 3 }
    local k04 = { round, 4 }
    local k05 = { round, 5 }
    local k06 = { round, 6 }
    local k07 = { round, 7 }
    local k08 = { round, 8 }
    assert(requests:send(round, 10) == true)
    sleep(0.003)
    local ack, ok = replies:recv(10)
    assert(ok == true and ack == round, "native GC worker reply timeout")
    assert(k01[1] == round and k01[2] == 1)
    assert(k02[1] == round and k02[2] == 2)
    assert(k03[1] == round and k03[2] == 3)
    assert(k04[1] == round and k04[2] == 4)
    assert(k05[1] == round and k05[2] == 5)
    assert(k06[1] == round and k06[2] == 6)
    assert(k07[1] == round and k07[2] == 7)
    assert(k08[1] == round and k08[2] == 8)
  end

  assert(requests:send("stop", 10) == true)
  harness.join_each({ worker }, function(result)
    assert(result == true)
  end, 30)
end

local function active_worker_root_stress()
  local nworkers = harness.env_number("LJ_M3_ACTIVE_ROOT_WORKERS", 4)
  local payload = harness.env_number("LJ_M3_ACTIVE_ROOT_PAYLOAD", 96)
  local churn = harness.env_number("LJ_M3_ACTIVE_ROOT_CHURN", 32)
  local ready, release = harness.channels(nworkers)
  local weak = setmetatable({}, { __mode = "v" })
  local workers = {}
  local old_gcworkers

  local ok_workers, old = pcall(th.gcworkers, 2)
  if ok_workers then old_gcworkers = old end

  for id = 1, nworkers do
    workers[id] = th.spawn(function(ready_ch, release_ch, worker_id, count)
      local root = {
	tag = "active-thread-root",
	id = worker_id,
	payload = {}
      }
      for i = 1, count do
	root.payload[i] = {
	  owner = worker_id,
	  slot = i,
	  text = "active-root:" .. worker_id .. ":" .. i
	}
      end
      local function checksum()
	local total = root.id
	for i = 1, #root.payload do
	  total = total + root.payload[i].slot
	end
	return total
      end

      assert(ready_ch:send({ worker_id, root }, 10) == true)
      local token, ok = release_ch:recv(20)
      assert(ok == true and token == "go")
      return checksum()
    end, ready, release, id, payload)
  end

  for _ = 1, nworkers do
    local msg, ok = ready:recv(10)
    assert(ok == true, "active root worker did not publish root")
    weak[msg[1]] = msg[2]
    msg = nil
  end

  for round = 1, 6 do
    collectgarbage(round % 2 == 0 and "collect" or "step")
    for i = 1, churn do
      local t = th.spawn(function(seed)
	local box = { seed = seed }
	return box.seed
      end, round * 100000 + i)
      local ok, result = t:join(10)
      assert(ok == true and result == round * 100000 + i)
      if i % 8 == 0 then collectgarbage("step") end
    end
    collectgarbage("collect")
    for id = 1, nworkers do
      local root = weak[id]
      assert(type(root) == "table", "active worker root was collected")
      assert(root.tag == "active-thread-root")
      assert(root.id == id)
      assert(type(root.payload[payload]) == "table")
    end
  end

  harness.release_start(release, nworkers, 10)
  harness.join_each(workers, function(result, id)
    assert(result == id + (payload * (payload + 1)) / 2)
  end, 30)

  if old_gcworkers ~= nil then
    assert(th.gcworkers(old_gcworkers) >= 0)
  end
end

assert_stdlib_roots()
collectgarbage("collect")
assert_stdlib_roots()

local sum = churn_closures(5000)
collectgarbage("collect")
assert_stdlib_roots()
native_leave_gc_preserves_caller_frame()

assert(sum == 12507500)
active_worker_root_stress()

print("t-gc-active-thread-roots OK")
