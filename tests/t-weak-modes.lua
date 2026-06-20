local ok_ffi, ffi = pcall(require, "ffi")
local ok_th, th = pcall(require, "threading")
local harness = require"thread_harness"

local function fullgc(n)
  for _ = 1, n or 2 do
    collectgarbage("collect")
  end
end

local function npairs(t)
  local n = 0
  for _ in pairs(t) do
    n = n + 1
  end
  return n
end

do
  local wv = setmetatable({}, { __mode = "v" })
  local keep = { tag = "keep" }
  wv.keep = keep
  wv.drop = { tag = "drop" }
  wv.str = "m8 weak strings stay strong"
  fullgc(3)
  assert(wv.keep == keep, "weak-value table cleared an anchored value")
  assert(wv.drop == nil, "weak-value table kept an unreachable value")
  assert(wv.str == "m8 weak strings stay strong",
	 "weak-value table cleared a string value")
end

do
  local wk = setmetatable({}, { __mode = "k" })
  local keepk = { tag = "keep-key" }
  local dropk = { tag = "drop-key" }
  local keepv = { tag = "keep-value" }
  wk[keepk] = keepv
  wk[dropk] = { tag = "drop-value" }
  wk["m8-string-key"] = { tag = "string-key-value" }
  dropk = nil
  fullgc(3)
  assert(wk[keepk] == keepv, "weak-key table cleared an anchored key")
  assert(wk["m8-string-key"] ~= nil, "weak-key table cleared a string key")
  assert(npairs(wk) == 2, "weak-key table kept an unreachable key")
end

do
  local wkv = setmetatable({}, { __mode = "kv" })
  local keepk = { tag = "keep-key" }
  local keepv = { tag = "keep-value" }
  local dropk = { tag = "drop-key" }
  local key_only = { tag = "key-only" }
  wkv[keepk] = keepv
  wkv[dropk] = { tag = "drop-value" }
  wkv[key_only] = { tag = "drop-value-with-live-key" }
  wkv["m8-string-key"] = "m8-string-value"
  dropk = nil
  key_only = nil
  fullgc(3)
  assert(wkv[keepk] == keepv, "weak-kv table cleared anchored entry")
  assert(wkv["m8-string-key"] == "m8-string-value",
	 "weak-kv table cleared string key/value entry")
  assert(npairs(wkv) == 2, "weak-kv table kept unreachable entries")
end

do
  local observer = setmetatable({}, { __mode = "v" })
  local function make_cycle()
    local a = setmetatable({}, { __mode = "kv" })
    local b = setmetatable({}, { __mode = "kv" })
    a[b] = b
    b[a] = a
    observer[1] = a
  end
  make_cycle()
  fullgc(3)
  assert(observer[1] == nil, "cycle of weak tables kept itself alive")
end

do
  local keep = {}
  local count = 0
  local dumped = string.dump(assert(loadstring(
    "return function(x) return x * 3 end"))())
  for i = 1, 320 do
    local chunk = assert(loadstring(
      "local n = " .. i .. " return function(x) return x + n end"))
    count = count + 1; keep[count] = chunk
    count = count + 1; keep[count] = chunk()
    count = count + 1; keep[count] = assert(loadstring(dumped))
    count = count + 1; keep[count] = assert(loadstring("return " .. i))
  end
  for i = 1, count, 4 do keep[i] = nil end
  for i = 1, 120 do
    keep[count + i] = { i, i + 1, i + 2 }
    for j = 4, 40 do keep[count + i][j] = j end
  end
  local wkv = setmetatable({}, { __mode = "kv" })
  do
    local k, v = {}, {}
    wkv[k] = v
  end
  collectgarbage("collect")
  assert(npairs(wkv) == 0, "weak-kv table kept a one-cycle hash entry")
end

if ok_ffi then
  ffi.cdef[[
  typedef struct { int x; } lj_m8_fin_obj_t;
  ]]
  local fin_t = ffi.typeof("lj_m8_fin_obj_t")

  do
    local order = {}
    local function make_finalizers()
      for i = 1, 3 do
	local id = i
	ffi.gc(fin_t(), function(_)
	  order[#order + 1] = id
	end)
      end
    end
    make_finalizers()
    fullgc(3)
    assert(table.concat(order, ",") == "3,2,1",
	   "ffi.gc finalizers did not run in reverse registration order")
  end

  do
    local fired = 0
    local function make_finalizers()
      ffi.gc(fin_t(), function(_)
	fired = fired + 1
	collectgarbage("collect")
      end)
      local cleared = ffi.gc(fin_t(), function(_)
	fired = fired + 100
      end)
      ffi.gc(cleared, nil)
    end
    make_finalizers()
    fullgc(3)
    assert(fired == 1, "ffi.gc finalizer clear or nested GC semantics broke")
    fullgc(2)
    assert(fired == 1, "ffi.gc finalizer ran more than once")
  end

  if ok_th and os.getenv("LJ_M8_FINALIZER_SPAWN") ~= "0" then
    local done = th.channel(1)
    local fired = 0
    local function make_finalizer()
      ffi.gc(fin_t(), function(_)
	fired = fired + 1
	local worker = th.spawn(function(q)
	  q:send("spawned")
	  return "joined"
	end, done)
	local ok, result = worker:join(10)
	assert(ok == true and result == "joined", tostring(result))
      end)
    end
    make_finalizer()
    fullgc(3)
    local msg, ok = done:recv(1)
    assert(ok == true and msg == "spawned", "finalizer-spawned thread did not run")
    assert(fired == 1, "finalizer-spawn case ran more than once")
  end
end

do
  local fired = 0
  local function make_table()
    setmetatable({}, { __gc = function() fired = fired + 1 end })
  end
  make_table()
  fullgc(3)
  assert(fired == 0, "Lua 5.1 table __gc unexpectedly ran")
end

local weak_race_iters = harness.env_number("LJ_M8_WEAK_RACE_ITERS", 512)
if ok_th and weak_race_iters > 0 then
  local stop = th.channel(1)
  local wt = setmetatable({}, { __mode = "v" })
  local n = weak_race_iters
  local worker = th.spawn(function(tbl, stop_ch, count)
    for i = 1, count do
      tbl[(i % 64) + 1] = { i }
      if i % 64 == 0 then
	local _, stopped = stop_ch:peek()
	if stopped then break end
      end
    end
    return true
  end, wt, stop, n)
  for _ = 1, 8 do
    collectgarbage("collect")
  end
  stop:send(true, 1)
  local joined, result = worker:join(10)
  assert(joined == true and result == true, tostring(result))
  fullgc(2)
  for k, v in pairs(wt) do
    assert(type(k) == "number", "weak writer left a non-number key")
    assert(type(v) == "table", "weak writer left a non-table value")
  end
end

print("t-weak-modes OK: weak modes, weak cycles, and current finalizer semantics verified")
