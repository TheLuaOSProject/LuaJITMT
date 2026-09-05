local th = require("threading")
local harness = require("thread_harness")

local rounds = harness.env_number("LJ_M5_TAB_ROOTED_READER_ROUNDS", 2400)
local width = 48
local shared = {}
local ready, start = harness.channels(3)
local workers = {}

local function pack(...)
  return { n = select("#", ...), ... }
end

for i = 1, width do
  shared[i] = i % 2 == 0 and i or ("seed:" .. i .. ":" .. string.rep("s", 96))
end

local function await_start(readych, startch)
  assert(readych:send(true, 10) == true)
  local _, ok = startch:recv(10)
  assert(ok == true)
end

workers[1] = th.spawn(function(tbl, n, count, readych, startch)
  await_start(readych, startch)
  for round = 1, count do
    local slot = 1 + (round % n)
    if round % 2 == 0 then
      tbl[slot] = round
    else
      tbl[slot] = "writer:" .. round .. ":" .. string.rep("w", 128 + round % 97)
    end
    local sparse = 1024 + (round % 257)
    tbl[sparse] = "resize:" .. round
    if round > 257 then tbl[1024 + ((round - 129) % 257)] = nil end
    if round % 41 == 0 then collectgarbage("step", 32) end
  end
  return true
end, shared, width, rounds, ready, start)

workers[2] = th.spawn(function(tbl, n, count, readych, startch)
  await_start(readych, startch)
  for round = 1, count do
    local values = pack(unpack(tbl, 1, n))
    assert(values.n == n, "unpack lost a rooted result")
    for i = 1, n do
      local ty = type(values[i])
      assert(ty == "string" or ty == "number",
             "unpack exposed a non-value table snapshot: " .. ty)
    end
    if round % 17 == 0 then collectgarbage("step", 16) end
  end
  return true
end, shared, width, rounds, ready, start)

workers[3] = th.spawn(function(tbl, n, count, readych, startch)
  await_start(readych, startch)
  for round = 1, count do
    local text = table.concat(tbl, "|", 1, n)
    assert(type(text) == "string" and #text >= n - 1,
           "table.concat lost a rooted string result")
    if round % 23 == 0 then collectgarbage("collect") end
  end
  return true
end, shared, width, rounds, ready, start)

harness.wait_ready(ready, #workers, 10, "rooted table readers")
harness.release_start(start, #workers, 10)
harness.join_all(workers, 40)

do
  local bad = {}
  local ok, err = pcall(table.concat, { "ok", bad }, ",", 1, 2)
  assert(ok == false)
  assert(tostring(err):match("invalid value %(table%) at index 2"), tostring(err))
end

do
  local a, b, c, d = unpack({ "a", nil, "c", 4 }, 1, 4)
  assert(a == "a" and b == nil and c == "c" and d == 4)
end

print("t-tab-rooted-readers OK: unpack/concat retain protected table results")
