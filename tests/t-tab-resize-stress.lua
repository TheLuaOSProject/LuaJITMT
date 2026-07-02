local th = require"threading"
local harness = require"thread_harness"

local reps = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_REPS", 768)
local writers = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_THREADS", 3)
local jit_reps = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_JIT_REPS", 2200)
local jit_read_reps =
  harness.env_number("LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS", jit_reps)
local traversal_rounds =
  harness.env_number("LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS", 192)
local finalizer_objects =
  harness.env_number("LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS", 192)
local key_objects =
  harness.env_number("LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS", 192)
local selected_cases = os.getenv("LJ_M5_TAB_RESIZE_STRESS_CASES")
local selected_traversal_modes =
  os.getenv("LJ_M5_TAB_RESIZE_TRAVERSAL_MODES")

local function assert_lua_value(v, label)
  label = label or "Lua value"
  local tv = type(v)
  assert(tv ~= "userdata", label .. " exposed an internal userdata sentinel")
  assert(tv ~= "cdata", label .. " exposed an internal cdata sentinel")
end

local function ready_start(n)
  return th.channel(n), th.channel(n)
end

local function case_filter(spec)
  if spec == nil or spec == "" then return nil end
  local out = {}
  for name in spec:gmatch("[^,%s]+") do out[name] = true end
  return out
end

local enabled_cases = case_filter(selected_cases)
local enabled_traversal_modes =
  case_filter(selected_traversal_modes) or
  { pairs = true, ipairs = true, next = true }

local function run_case(name, fn)
  if enabled_cases == nil or enabled_cases[name] then
    fn()
    return 1
  end
  return 0
end

local function collect_while_working(rounds)
  for i = 1, rounds do
    collectgarbage(i % 3 == 0 and "collect" or "step")
    if i % 8 == 0 then th.sleep(0.001) end
  end
end

local function weak_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local k = id * 1000000 + i
    tbl[k] = { id, i }
    tbl[-k] = i
    if i > 4 and i % 5 == 0 then tbl[k - 4] = nil end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_weak_clear_resize()
  local weak = setmetatable({}, { __mode = "v" })
  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] = th.spawn(weak_resize_writer, weak, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "weak resize")
  harness.release_start(start, writers, 10)
  collect_while_working(96)
  harness.join_all(workers, 30)
  harness.fullgc(2)
  for k, v in pairs(weak) do
    assert(type(k) == "number", "weak resize left non-number key")
    local tv = type(v)
    assert(tv == "number" or tv == "table",
	   "weak resize left unexpected value type " .. tv)
    assert_lua_value(v, "weak resize")
  end
end

local function strong_resize_writer(strong, observer, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    strong[id * 1000000 + i] = i
    if i % 32 == 0 then
      local slot = id * 100 + (i / 32)
      local obj = { owner = id, round = i }
      strong["keep:" .. slot] = obj
      observer[slot] = obj
    end
    if i > 128 and i % 7 == 0 then
      strong[id * 1000000 + i - 96] = nil
    end
    if i % 96 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_gc_mark_resize()
  local strong = {}
  local observer = setmetatable({}, { __mode = "v" })
  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] = th.spawn(strong_resize_writer, strong, observer,
			  ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "strong resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(3)
  for id = 1, writers do
    for i = 32, reps, 32 do
      local slot = id * 100 + (i / 32)
      local obj = strong["keep:" .. slot]
      assert(type(obj) == "table", "strong resize lost table-owned object")
      assert(observer[slot] == obj,
	     "GC missed table-owned object during resize forwarding")
      assert_lua_value(obj, "strong resize")
    end
  end
end

local function object_key_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl["resize-key:" .. id .. ":" .. i] = i
    tbl[id * 1000000 + i] = i
    if i > 64 and i % 8 == 0 then
      tbl["resize-key:" .. id .. ":" .. (i - 32)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_gc_key_resize()
  local t = {}
  local weak_keys = setmetatable({}, { __mode = "v" })
  local weak_vals = setmetatable({}, { __mode = "v" })
  local ready, start = ready_start(writers)
  local workers = {}
  local n = key_objects

  for i = 1, n do
    local key = { kind = "resize-key", id = i }
    local val = { kind = "resize-value", id = i }
    t[key] = val
    weak_keys[i] = key
    weak_vals[i] = val
  end

  for i = 1, writers do
    workers[i] =
      th.spawn(object_key_resize_writer, t, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "GC object key resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(3)

  for i = 1, n do
    local key = weak_keys[i]
    local val = weak_vals[i]
    assert(type(key) == "table", "GC missed table-owned hash key during resize")
    assert(type(val) == "table", "GC missed object-keyed value during resize")
    assert(t[key] == val, "object-keyed slot changed during resize")
    assert_lua_value(key, "object-key resize")
    assert_lua_value(val, "object-key resize")
  end
end

local function weak_key_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  local roots = {}
  for i = 1, n do
    local key = { kind = "weak-resize-key", owner = id, round = i }
    tbl[key] = { owner = id, round = i }
    tbl["weak-key-grow:" .. id .. ":" .. i] = i
    if i % 16 == 0 then roots[#roots + 1] = key end
    if #roots > 32 then roots[#roots - 31] = nil end
    if i > 64 and i % 7 == 0 then
      tbl["weak-key-grow:" .. id .. ":" .. (i - 32)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_weak_key_resize()
  local weak = setmetatable({}, { __mode = "k" })
  local live_keys = {}
  local weak_vals = setmetatable({}, { __mode = "v" })
  local n = key_objects

  for i = 1, n do
    local key = { kind = "rooted-weak-key", id = i }
    local val = { kind = "weak-key-value", id = i }
    weak[key] = val
    weak_vals[i] = val
    if i % 2 == 1 then live_keys[i] = key end
  end

  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] =
      th.spawn(weak_key_resize_writer, weak, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "weak-key resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(4)

  for i = 1, n do
    if i % 2 == 1 then
      local key = live_keys[i]
      local val = weak[key]
      assert(type(key) == "table", "weak-key resize lost rooted key")
      assert(type(val) == "table",
	     "weak-key resize lost rooted weak-key entry")
      assert(weak_vals[i] == val,
	     "weak-key resize failed to keep rooted entry value live")
      assert_lua_value(key, "weak-key resize")
      assert_lua_value(val, "weak-key resize")
    else
      assert(weak_vals[i] == nil,
	     "weak-key resize kept value for unrooted collected key")
    end
  end
end

local function weak_key_meta_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  local roots = {}
  for i = 1, n do
    local key = { kind = "weak-meta-key", owner = id, round = i }
    tbl[key] = { owner = id, round = i }
    tbl["weak-meta-grow:" .. id .. ":" .. i] = i
    if i % 12 == 0 then roots[#roots + 1] = key end
    if #roots > 24 then roots[#roots - 23] = nil end
    if i % 31 == 0 then
      local probe = tbl.resize_meta_probe
      assert(type(probe) == "table" and probe.tag == "weak-meta-probe",
	     "weak-key metatable __index probe changed during resize")
    end
    if i > 64 and i % 7 == 0 then
      tbl["weak-meta-grow:" .. id .. ":" .. (i - 32)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_weak_key_metatable_resize()
  local fallback = { resize_meta_probe = { tag = "weak-meta-probe" } }
  local mt = { __mode = "k", __index = fallback }
  local weak = setmetatable({}, mt)
  local weak_mt = setmetatable({}, { __mode = "v" })
  local weak_fallback = setmetatable({}, { __mode = "v" })
  local live_keys = {}
  local weak_vals = setmetatable({}, { __mode = "v" })
  local n = key_objects

  weak_mt[1] = mt
  weak_fallback[1] = fallback
  weak_fallback[2] = fallback.resize_meta_probe

  for i = 1, n do
    local key = { kind = "rooted-weak-meta-key", id = i }
    local val = { kind = "weak-meta-value", id = i }
    weak[key] = val
    weak_vals[i] = val
    if i % 2 == 1 then live_keys[i] = key end
  end

  fallback = nil
  mt = nil

  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] =
      th.spawn(weak_key_meta_resize_writer, weak, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "weak-key metatable resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(4)

  local kept_mt = weak_mt[1]
  local kept_fallback = weak_fallback[1]
  local kept_probe = weak_fallback[2]
  assert(type(kept_mt) == "table",
	 "GC missed weak-key table metatable during resize forwarding")
  assert(type(kept_fallback) == "table",
	 "GC missed weak-key table __index during resize forwarding")
  assert(type(kept_probe) == "table",
	 "GC missed weak-key table __index probe during resize forwarding")
  assert(getmetatable(weak) == kept_mt,
	 "weak-key table metatable changed during resize")
  assert(weak.resize_meta_probe == kept_probe,
	 "weak-key metatable __index probe changed after resize")

  for i = 1, n do
    if i % 2 == 1 then
      local key = live_keys[i]
      local val = weak[key]
      assert(type(key) == "table", "weak-meta resize lost rooted key")
      assert(type(val) == "table",
	     "weak-meta resize lost rooted weak-key entry")
      assert(weak_vals[i] == val,
	     "weak-meta resize failed to keep rooted entry value live")
      assert_lua_value(key, "weak-key metatable resize")
      assert_lua_value(val, "weak-key metatable resize")
    else
      assert(weak_vals[i] == nil,
	     "weak-meta resize kept value for unrooted collected key")
    end
  end
end

local function finalizer_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl["resize-fin:" .. id .. ":" .. i] = i
    tbl[id * 1000000 + i] = i
    if i > 32 and i % 6 == 0 then
      tbl["resize-fin:" .. id .. ":" .. (i - 16)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_finalizer_resize()
  local okffi, ffi = pcall(require, "ffi")
  if not okffi then return end

  ffi.cdef[[
  typedef struct { int id; } lj_m5_tab_resize_fin_t;
  ]]

  local ctype = ffi.typeof("lj_m5_tab_resize_fin_t")
  local strong = {}
  local weak = setmetatable({}, { __mode = "v" })
  local finalized = {}
  local n = finalizer_objects

  for i = 1, n do
    local obj = ffi.gc(ctype(i), function(cd)
      finalized[tonumber(cd.id)] = true
    end)
    strong["fin:" .. i] = obj
    weak[i] = obj
  end

  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] =
      th.spawn(finalizer_resize_writer, strong, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "finalizer resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(3)

  for i = 1, n do
    local obj = strong["fin:" .. i]
    assert(type(obj) == "cdata", "finalizer resize lost table-held cdata")
    assert(weak[i] == obj,
	   "GC missed table-held finalizer cdata during resize forwarding")
    assert(finalized[i] ~= true,
	   "table-held finalizer cdata was finalized during resize")
  end
end

local function metatable_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl["resize-mt:" .. id .. ":" .. i] = i
    tbl[id * 1000000 + i] = i
    if i % 37 == 0 then
      local probe = tbl.resize_meta_probe
      assert(type(probe) == "table" and probe.tag == "resize-metatable",
	     "metatable __index probe changed during resize")
    end
    if i > 48 and i % 9 == 0 then
      tbl["resize-mt:" .. id .. ":" .. (i - 24)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_metatable_resize()
  local t = {}
  local weak = setmetatable({}, { __mode = "v" })
  local probe = { tag = "resize-metatable" }
  local mt = { __index = { resize_meta_probe = probe } }
  weak[1] = mt
  weak[2] = probe
  setmetatable(t, mt)
  mt = nil
  probe = nil

  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] =
      th.spawn(metatable_resize_writer, t, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "metatable resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(3)

  local kept_mt = weak[1]
  local kept_probe = weak[2]
  assert(type(kept_mt) == "table",
	 "GC missed table metatable during resize forwarding")
  assert(type(kept_probe) == "table",
	 "GC missed table metatable __index edge during resize forwarding")
  assert(getmetatable(t) == kept_mt, "table metatable changed during resize")
  assert(t.resize_meta_probe == kept_probe,
	 "metatable __index probe changed after resize")
end

local function jit_store_worker(tbl, ready, start, key, n)
  local okjit, jitmod = pcall(require, "jit")
  if okjit and jitmod.status() then
    jitmod.opt.start("hotloop=1", "hotexit=1")
  end
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl[key] = i
    if i % 257 == 0 then tbl[key + 64] = nil end
  end
  return true
end

local function exercise_jit_store_resize()
  local t = {}
  for i = 1, 256 do t[i] = 0 end
  local nworkers = writers
  local ready, start = ready_start(nworkers)
  local workers = {}
  for i = 1, nworkers do
    workers[i] = th.spawn(jit_store_worker, t, ready, start, i, jit_reps)
  end
  harness.wait_ready(ready, nworkers, 10, "jit resize")
  harness.release_start(start, nworkers, 10)
  for i = 1, reps do
    local k = 10000 + i
    t[k] = i
    if i % 3 == 0 then t[k - 1] = nil end
    if i % 32 == 0 then collectgarbage("step") end
  end
  harness.join_all(workers, 30)
  for i = 1, nworkers do
    assert(t[i] == jit_reps, "traced store lost update across resize")
    assert_lua_value(t[i], "jit resize")
  end
  harness.fullgc(2)
end

local function jit_read_worker(tbl, ready, start, array_key, hash_key, want, n)
  local okjit, jitmod = pcall(require, "jit")
  if okjit and jitmod.status() then
    jitmod.opt.start("hotloop=1", "hotexit=1")
  end
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local av = tbl[array_key]
    local hv = tbl[hash_key]
    assert(av == want, "traced array read changed across resize")
    assert(hv == want, "traced hash read changed across resize")
    assert(type(av) ~= "userdata" and type(av) ~= "cdata",
	   "traced array read exposed an internal sentinel")
    assert(type(hv) ~= "userdata" and type(hv) ~= "cdata",
	   "traced hash read exposed an internal sentinel")
    if i % 251 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_jit_read_resize()
  local t = {}
  local nreaders = writers
  local ready, start = ready_start(nreaders)
  local workers = {}
  for i = 1, nreaders do
    t[i] = i
    t["read:" .. i] = i
  end
  for i = 1, nreaders do
    workers[i] = th.spawn(jit_read_worker, t, ready, start, i,
			  "read:" .. i, i, jit_read_reps)
  end
  harness.wait_ready(ready, nreaders, 10, "jit read resize")
  harness.release_start(start, nreaders, 10)
  for i = 1, reps do
    local k = 20000 + i
    t[k] = i
    t["grow:" .. i] = i
    if i > 8 and i % 5 == 0 then t["grow:" .. (i - 4)] = nil end
    if i % 32 == 0 then collectgarbage("step") end
  end
  harness.join_all(workers, 30)
  for i = 1, nreaders do
    assert(t[i] == i, "stable array key changed after traced resize reads")
    assert(t["read:" .. i] == i, "stable hash key changed after traced resize reads")
  end
  harness.fullgc(2)
end

local function jit_iter_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local ak = 512 + id * 1000000 + i
    local hk = "jit-iter-grow:" .. id .. ":" .. i
    tbl[ak] = { owner = id, round = i }
    tbl[hk] = i
    if i > 32 and i % 5 == 0 then
      tbl[ak - 16] = nil
      tbl["jit-iter-grow:" .. id .. ":" .. (i - 24)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function jit_iter_observer(tbl, ready, start, id, rounds)
  local okjit, jitmod = pcall(require, "jit")
  if okjit and jitmod.status() then
    jitmod.flush()
    jitmod.opt.start("hotloop=1", "hotexit=1")
  end

  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for round = 1, rounds do
    local anchor = tbl.anchor
    if type(anchor) ~= "table" or anchor.tag ~= "jit-iter-anchor" then
      return false, "JIT iterator resize lost stable hash anchor"
    end

    local count = 0
    for k, v in pairs(tbl) do
      assert_lua_value(k, "JIT pairs resize key")
      assert_lua_value(v, "JIT pairs resize value")
      count = count + 1
      if count >= 192 then break end
    end

    count = 0
    for i, v in ipairs(tbl) do
      if type(i) ~= "number" then
	return false, "JIT ipairs resize returned non-number index"
      end
      assert_lua_value(v, "JIT ipairs resize value")
      count = count + 1
      if count >= 96 then break end
    end
    if count < 64 then
      return false, "JIT ipairs resize crossed stable prefix"
    end

    local k, v = next(tbl, nil)
    if k ~= nil then
      assert_lua_value(k, "JIT next resize key")
      assert_lua_value(v, "JIT next resize value")
    end

    if round % 16 == 0 then
      collectgarbage("step")
      if id == 1 then th.sleep(0.001) end
    end
  end
  return true
end

local function exercise_jit_iterator_resize()
  local t = { anchor = { tag = "jit-iter-anchor" } }
  local observers = 2
  local nworkers = writers + observers
  local ready, start = ready_start(nworkers)
  local workers = {}

  for i = 1, 128 do t[i] = { kind = "jit-iter-prefix", slot = i } end
  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(jit_iter_resize_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(jit_iter_observer, t, ready, start, i, traversal_rounds)
  end

  harness.wait_ready(ready, nworkers, 10, "JIT iterator resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result, _, msg)
    assert(result == true, tostring(msg or result))
  end, 30)

  harness.fullgc(3)
  assert(type(t.anchor) == "table" and t.anchor.tag == "jit-iter-anchor",
	 "JIT iterator resize changed stable hash anchor")
  for i = 1, 64 do
    local v = t[i]
    assert(type(v) == "table",
	   "JIT iterator resize changed stable array prefix")
    assert_lua_value(v, "JIT iterator resize prefix")
  end
end

local function len_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local slot = 128 + ((i + id * 17) % 512) + 1
    tbl[slot] = slot
    tbl[id * 1000000 + i] = i
    tbl["len-grow:" .. id .. ":" .. i] = i
    if i > 32 and i % 5 == 0 then
      tbl[160 + ((i + id * 7) % 480)] = nil
    end
    if i > 64 and i % 7 == 0 then
      tbl["len-grow:" .. id .. ":" .. (i - 32)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function len_observer(tbl, ready, start, id, rounds)
  local function check_len(n, label)
    if type(n) ~= "number" then
      return label .. " returned non-number length"
    end
    if n ~= math.floor(n) then
      return label .. " returned non-integer length"
    end
    if n < 128 then
      return label .. " crossed below stable prefix"
    end
    return nil
  end

  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for round = 1, rounds do
    local err = check_len(#tbl, "# resize")
    if err then return nil, err end
    if rawlen then
      err = check_len(rawlen(tbl), "rawlen resize")
      if err then return nil, err end
    end
    if round % 8 == 0 then
      local ok_concat, s = pcall(table.concat, tbl, "", 1, 16)
      if not ok_concat or type(s) ~= "string" then
	return nil, "table.concat stable-prefix probe failed"
      end
    end
    if round % 16 == 0 then
      collectgarbage("step")
      if id == 1 then th.sleep(0.001) end
    end
  end
  return true
end

local function exercise_len_resize()
  local t = {}
  local observers = 2
  local nworkers = writers + observers
  local ready, start = ready_start(nworkers)
  local workers = {}

  for i = 1, 128 do t[i] = i end
  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(len_resize_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(len_observer, t, ready, start, i, traversal_rounds)
  end
  harness.wait_ready(ready, nworkers, 10, "length resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(96)
  harness.join_each(workers, function(result, _, msg)
    assert(result == true, tostring(msg or result))
  end, 30)
  for i = 1, 128 do
    assert(t[i] == i, "length resize changed stable prefix")
  end
  harness.fullgc(2)
end

local function traversal_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl[i] = i
    tbl[id * 1000000 + i] = { id = id, round = i }
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function traversal_observer(tbl, ready, start, id, rounds)
  local function check(v, label)
    local tv = type(v)
    if tv == "userdata" then
      return label .. " exposed an internal userdata sentinel"
    end
    if tv == "cdata" then
      return label .. " exposed an internal cdata sentinel"
    end
    return nil
  end

  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for round = 1, rounds do
    if enabled_traversal_modes.pairs then
      local count = 0
      for k, v in pairs(tbl) do
	local err = check(k, "pairs traversal")
	if err then return nil, err end
	err = check(v, "pairs traversal")
	if err then return nil, err end
	count = count + 1
	if count >= 256 then break end
      end
    end

    if enabled_traversal_modes.ipairs then
      local count = 0
      for i, v in ipairs(tbl) do
	if type(i) ~= "number" then
	  return nil, "ipairs returned non-number index"
	end
	local err = check(v, "ipairs traversal")
	if err then return nil, err end
	count = count + 1
	if count >= 256 then break end
      end
    end

    if enabled_traversal_modes.next then
      local k, v = next(tbl, nil)
      local count = 0
      while k ~= nil and count < 32 do
	local err = check(k, "next(nil) traversal")
	if err then return nil, err end
	err = check(v, "next(nil) traversal")
	if err then return nil, err end
	k, v = next(tbl, k)
	count = count + 1
      end
    end

    if round % 16 == 0 then
      collectgarbage("step")
      if id == 1 then th.sleep(0.001) end
    end
  end
  return true
end

local function exercise_concurrent_traversal_resize()
  local t = {}
  local observers = 2
  local nworkers = writers + observers
  local ready, start = ready_start(nworkers)
  local workers = {}

  for i = 1, 64 do t[i] = i end
  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(traversal_resize_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(traversal_observer, t, ready, start, i, traversal_rounds)
  end
  harness.wait_ready(ready, nworkers, 10, "traversal resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(96)
  harness.join_each(workers, function(result, _, msg)
    assert(result == true, tostring(msg or result))
  end, 30)
  for i = 1, math.min(reps, 128) do
    assert(type(t[i]) == "number", "array traversal resize lost numeric slot")
    assert_lua_value(t[i], "concurrent traversal resize")
  end
  harness.fullgc(2)
end

local function next_churn_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local ak = i + id * 4096
    local hk = "next-churn:" .. id .. ":" .. i
    tbl[ak] = { owner = id, round = i }
    tbl[hk] = i
    if i > 8 and i % 3 == 0 then tbl[ak - 6] = nil end
    if i > 16 and i % 5 == 0 then
      tbl["next-churn:" .. id .. ":" .. (i - 9)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function next_churn_observer(tbl, ready, start, id, rounds)
  local function check(k, v, label)
    if k ~= nil then
      assert_lua_value(k, label .. " key")
      assert_lua_value(v, label .. " value")
    end
  end

  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for round = 1, rounds do
    local k, v = next(tbl, nil)
    check(k, v, "next churn")

    local count = 0
    for pk, pv in pairs(tbl) do
      check(pk, pv, "pairs churn")
      count = count + 1
      if count >= 96 then break end
    end

    tbl["observer:" .. id .. ":" .. round] = round
    if round > 12 and round % 4 == 0 then
      tbl["observer:" .. id .. ":" .. (round - 8)] = nil
    end
    if round % 16 == 0 then
      collectgarbage("step")
      if id == 1 then th.sleep(0.001) end
    end
  end
  return true
end

local function exercise_next_churn_resize()
  local t = {}
  local live_keys = {}
  local live_vals = {}
  local weak_keys = setmetatable({}, { __mode = "v" })
  local weak_vals = setmetatable({}, { __mode = "v" })
  local observers = 2
  local nworkers = writers + observers
  local ready, start = ready_start(nworkers)
  local workers = {}
  local n = math.min(key_objects, 96)

  for i = 1, n do
    local key = { kind = "next-churn-key", id = i }
    local val = { kind = "next-churn-value", id = i }
    t[key] = val
    live_keys[i] = key
    live_vals[i] = val
    weak_keys[i] = key
    weak_vals[i] = val
  end
  for i = 1, 64 do
    t[i] = i
    t["seed:" .. i] = i
  end

  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(next_churn_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(next_churn_observer, t, ready, start, i, traversal_rounds)
  end
  harness.wait_ready(ready, nworkers, 10, "next churn resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result, _, msg)
    assert(result == true, tostring(msg or result))
  end, 30)
  harness.fullgc(3)

  for i = 1, n do
    local key = live_keys[i]
    local val = live_vals[i]
    assert(type(weak_keys[i]) == "table",
	   "next churn resize lost rooted table key")
    assert(type(weak_vals[i]) == "table",
	   "next churn resize lost rooted table value")
    assert(t[key] == val, "next churn resize changed rooted object slot")
    assert_lua_value(key, "next churn resize")
    assert_lua_value(val, "next churn resize")
  end
end

local function next_invalid_cursor_worker(ready, release)
  assert(ready:send(true, 10) == true)
  local _, ok = release:recv(10)
  assert(ok == true)
  return true
end

local function exercise_next_invalid_cursor_boundary()
  local ready, release = ready_start(1)
  local worker = th.spawn(next_invalid_cursor_worker, ready, release)
  local key = { kind = "next-invalid-cursor" }
  local t = { live = true }

  harness.wait_ready(ready, 1, 10, "next invalid cursor")

  local ok, k, v = pcall(next, t, key)
  assert(ok == true, tostring(k))
  assert(k == nil and v == nil,
	 "live MT invalid-cursor next() should terminate traversal")

  assert(release:send("go", 10) == true)
  harness.join_each({ worker }, function(result)
    assert(result == true)
  end, 10)

  ok, k = pcall(next, t, key)
  assert(ok == false and tostring(k):match("invalid key"),
	 "single-thread invalid-cursor next() must keep stock error semantics")
end

local function tablelib_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local k = "tablelib-grow:" .. id .. ":" .. i
    tbl[k] = i
    if i > 24 and i % 5 == 0 then
      tbl["tablelib-grow:" .. id .. ":" .. (i - 16)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function tablelib_clear_worker(tbl, ready, start, n)
  local table_clear = table.clear or require"table.clear"
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    table_clear(tbl)
    tbl["tableclear-round"] = i
    for j = 1, 6 do
      tbl["tableclear-grow:" .. i .. ":" .. j] = { round = i, slot = j }
    end
    if i % 16 == 0 then collectgarbage("step") end
  end
  local final = { kind = "tablelib-clear-final", rounds = n }
  tbl["tableclear-final"] = final
  return final
end

local function exercise_table_clear_resize()
  local t = {}
  local weak_final = setmetatable({}, { __mode = "v" })
  local observers = 2
  local clear_rounds = math.min(reps, 192)
  local nworkers = writers + observers + 1
  local ready, start = ready_start(nworkers)
  local workers = {}
  local final_marker

  for i = 1, 128 do
    t[i] = { kind = "tableclear-seed", slot = i }
    t["tableclear-seed:" .. i] = i
  end

  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(tablelib_resize_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(next_churn_observer, t, ready, start, i, traversal_rounds)
  end
  workers[#workers + 1] =
    th.spawn(tablelib_clear_worker, t, ready, start, clear_rounds)

  harness.wait_ready(ready, nworkers, 10, "table.clear resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result, _, msg)
    if result == true then return end
    assert(type(result) == "table" and
	   result.kind == "tablelib-clear-final", tostring(msg or result))
    final_marker = result
    weak_final[1] = result
  end, 30)

  assert(type(final_marker) == "table", "table.clear worker returned no marker")
  final_marker = nil
  harness.fullgc(3)
  assert(type(t["tableclear-final"]) == "table",
	 "table.clear post-clear marker was lost across resize")
  assert(weak_final[1] == t["tableclear-final"],
	 "GC missed table.clear post-clear marker after resize")
  local seen = 0
  for k, v in pairs(t) do
    assert_lua_value(k, "table.clear resize key")
    assert_lua_value(v, "table.clear resize value")
    seen = seen + 1
    if seen >= 256 then break end
  end
end

local function tablelib_insert_worker(tbl, ready, start, id, n)
  local inserted = {}
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local marker = {
      kind = "tablelib-insert",
      token = "tablelib:" .. id .. ":" .. i
    }
    if i % 5 == 0 then
      table.insert(tbl, marker)
    else
      table.insert(tbl, 1, marker)
    end
    inserted[#inserted + 1] = marker
    if i % 32 == 0 then collectgarbage("step") end
  end
  return inserted
end

local function exercise_table_library_resize()
  local t = {}
  local weak_inserted = setmetatable({}, { __mode = "v" })
  local tokens = {}
  local insert_rounds = math.min(reps, 384)
  local nworkers = writers + 1
  local ready, start = ready_start(nworkers)
  local workers = {}

  for i = 1, 64 do
    t[i] = { seed = i }
  end

  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(tablelib_resize_writer, t, ready, start, i, reps)
  end
  workers[#workers + 1] =
    th.spawn(tablelib_insert_worker, t, ready, start, 1, insert_rounds)

  harness.wait_ready(ready, nworkers, 10, "table library resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result)
    if result == true then return end
    assert(type(result) == "table", "unexpected tablelib worker result")
    for i = 1, #result do
      local marker = result[i]
      assert(type(marker) == "table" and marker.kind == "tablelib-insert")
      tokens[#tokens + 1] = marker.token
      weak_inserted[marker.token] = marker
    end
  end, 30)

  harness.fullgc(3)

  local found = {}
  for k, v in pairs(t) do
    assert_lua_value(k, "table library resize key")
    assert_lua_value(v, "table library resize value")
    if type(v) == "table" and v.kind == "tablelib-insert" then
      found[v.token] = true
    end
  end

  assert(#tokens == insert_rounds, "table.insert worker returned short list")
  for i = 1, #tokens do
    local token = tokens[i]
    assert(type(weak_inserted[token]) == "table",
	   "table.insert marker was not kept live by the table")
    assert(found[token], "table.insert marker missing after resize churn")
  end
end

local function tablelib_shift_worker(tbl, ready, start, id, n)
  local moves = 0
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local pos = 32 + ((i + id * 11) % 64)
    local value = table.remove(tbl, pos)
    if value == nil then
      return nil, "table.remove returned nil inside dense resize table"
    end
    assert_lua_value(value, "table.remove resize value")
    table.insert(tbl, pos, value)
    if table.move and i % 3 == 0 then
      local from = 1 + ((i + id * 7) % 24)
      local to = from + 15
      local dest = 96 + ((i + id * 5) % 24)
      table.move(tbl, from, to, dest, tbl)
      moves = moves + 1
    end
    if i % 32 == 0 then collectgarbage("step") end
  end
  return true, moves
end

local function exercise_table_library_shift_resize()
  local t = {}
  local nworkers = writers + 1
  local ready, start = ready_start(nworkers)
  local workers = {}
  local shift_rounds = math.min(reps, 384)

  for i = 1, 192 do
    t[i] = { kind = "tablelib-shift", slot = i }
  end

  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(tablelib_resize_writer, t, ready, start, i, reps)
  end
  workers[#workers + 1] =
    th.spawn(tablelib_shift_worker, t, ready, start, 1, shift_rounds)

  harness.wait_ready(ready, nworkers, 10, "table library shift resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result, moves, msg)
    if result == true then
      if moves ~= nil then
	assert(type(moves) == "number" and moves > 0,
	       "table.move did not run during table-library shift stress")
      end
      return
    end
    assert(false, tostring(msg or result))
  end, 30)

  harness.fullgc(3)

  for i = 1, 128 do
    local v = t[i]
    assert(type(v) == "table",
	   "table-library shift resize left sparse or non-table prefix")
    assert_lua_value(v, "table-library shift resize prefix")
  end
  local seen = 0
  for k, v in pairs(t) do
    assert_lua_value(k, "table-library shift resize key")
    assert_lua_value(v, "table-library shift resize value")
    seen = seen + 1
    if seen >= 256 then break end
  end
end

local function metadispatch_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local ak = 256 + id * 1000000 + i
    local hk = "metadispatch-grow:" .. id .. ":" .. i
    tbl[ak] = { owner = id, round = i }
    tbl[hk] = i
    if i > 32 and i % 5 == 0 then
      tbl[ak - 16] = nil
      tbl["metadispatch-grow:" .. id .. ":" .. (i - 24)] = nil
    end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function metadispatch_observer(tbl, ready, start, id, rounds)
  local inserted = {}
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for round = 1, rounds do
    local probe = tbl.resize_meta_dispatch_probe
    if type(probe) ~= "table" or probe.tag ~= "resize-metadispatch" then
      return nil, "__index dispatch changed during resize"
    end
    assert_lua_value(probe, "__index dispatch resize")

    local key = "metadispatch-new:" .. id .. ":" .. round
    local marker = { kind = "metadispatch-new", owner = id, round = round }
    tbl[key] = marker
    if rawget(tbl, key) ~= marker then
      return nil, "__newindex dispatch failed to publish unique key"
    end
    inserted[#inserted + 1] = marker

    if round % 12 == 0 then
      local count = 0
      for k, v in pairs(tbl) do
	assert_lua_value(k, "metadispatch traversal key")
	assert_lua_value(v, "metadispatch traversal value")
	count = count + 1
	if count >= 96 then break end
      end
    end
    if round % 16 == 0 then
      collectgarbage("step")
      if id == 1 then th.sleep(0.001) end
    end
  end
  return inserted
end

local function exercise_metamethod_dispatch_resize()
  local t = {}
  local probe = { tag = "resize-metadispatch" }
  local fallback = { resize_meta_dispatch_probe = probe }
  local mt = {
    __index = fallback,
    __newindex = function(self, key, value)
      rawset(self, key, value)
    end
  }
  local weak = setmetatable({}, { __mode = "v" })
  local weak_inserted = setmetatable({}, { __mode = "v" })
  local tokens = {}
  local observers = 2
  local nworkers = writers + observers
  local ready, start = ready_start(nworkers)
  local workers = {}

  weak[1] = mt
  weak[2] = fallback
  weak[3] = probe
  setmetatable(t, mt)
  mt = nil
  fallback = nil
  probe = nil

  for i = 1, 96 do t[i] = { kind = "metadispatch-prefix", slot = i } end
  for i = 1, writers do
    workers[#workers + 1] =
      th.spawn(metadispatch_resize_writer, t, ready, start, i, reps)
  end
  for i = 1, observers do
    workers[#workers + 1] =
      th.spawn(metadispatch_observer, t, ready, start, i, traversal_rounds)
  end

  harness.wait_ready(ready, nworkers, 10, "metamethod dispatch resize")
  harness.release_start(start, nworkers, 10)
  collect_while_working(128)
  harness.join_each(workers, function(result, _, msg)
    if result == true then return end
    assert(type(result) == "table", tostring(msg or result))
    for i = 1, #result do
      local marker = result[i]
      local token = marker.owner .. ":" .. marker.round
      tokens[#tokens + 1] = token
      weak_inserted[token] = marker
    end
  end, 30)

  harness.fullgc(3)

  assert(type(weak[1]) == "table",
	 "GC missed metatable during metamethod resize dispatch")
  assert(type(weak[2]) == "table",
	 "GC missed __index table during metamethod resize dispatch")
  assert(type(weak[3]) == "table",
	 "GC missed __index value during metamethod resize dispatch")
  assert(t.resize_meta_dispatch_probe == weak[3],
	 "__index dispatch changed after resize")
  for i = 1, #tokens do
    local token = tokens[i]
    assert(type(weak_inserted[token]) == "table",
	   "__newindex marker was not kept live by resized table")
  end
  for i = 1, 64 do
    local v = t[i]
    assert(type(v) == "table",
	   "metamethod dispatch resize changed stable prefix")
    assert_lua_value(v, "metamethod dispatch resize prefix")
  end
end

local ran = 0
ran = ran + run_case("weak", exercise_weak_clear_resize)
ran = ran + run_case("gcmark", exercise_gc_mark_resize)
ran = ran + run_case("gckey", exercise_gc_key_resize)
ran = ran + run_case("weakkey", exercise_weak_key_resize)
ran = ran + run_case("weakmeta", exercise_weak_key_metatable_resize)
ran = ran + run_case("finalizer", exercise_finalizer_resize)
ran = ran + run_case("metatable", exercise_metatable_resize)
ran = ran + run_case("jitstore", exercise_jit_store_resize)
ran = ran + run_case("jitread", exercise_jit_read_resize)
ran = ran + run_case("jititer", exercise_jit_iterator_resize)
ran = ran + run_case("len", exercise_len_resize)
ran = ran + run_case("traversal", exercise_concurrent_traversal_resize)
ran = ran + run_case("nextchurn", exercise_next_churn_resize)
ran = ran + run_case("nextinvalid", exercise_next_invalid_cursor_boundary)
ran = ran + run_case("tableclear", exercise_table_clear_resize)
ran = ran + run_case("tablelib", exercise_table_library_resize)
ran = ran + run_case("tablelibshift", exercise_table_library_shift_resize)
ran = ran + run_case("metadispatch", exercise_metamethod_dispatch_resize)
assert(ran > 0, "no table resize stress cases selected")

print(("t-tab-resize-stress OK: %d writers, %d resize rounds"):format(
  writers, reps))
