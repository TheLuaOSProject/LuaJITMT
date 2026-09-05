local th = require"threading"

local workers_n = tonumber(os.getenv("LJ_M4_REQUIRE_WORKERS") or "") or 8

local function recv_expect(ch, timeout, what)
  local value, ok = ch:recv(timeout)
  assert(ok == true, what .. " timed out")
  return value
end

local function wait_ready(ch, n)
  for _ = 1, n do
    assert(recv_expect(ch, 10, "worker ready") == "ready")
  end
end

local function release_all(ch, n)
  for _ = 1, n do
    assert(ch:send("go", 10) == true)
  end
end

local function join_all(threads)
  local out = {}
  for i = 1, #threads do
    out[i] = { threads[i]:join(10) }
    assert(out[i][1] ~= nil, "worker join timed out")
    assert(out[i][1] == true, tostring(out[i][2]))
  end
  return out
end

local function concurrent_success_once()
  local name = "threading_require_once_success"
  local ready = th.channel(workers_n)
  local start = th.channel(workers_n)
  local entered = th.channel(workers_n)
  local release = th.channel(workers_n)
  local body_count = 0
  local threads = {}

  package.loaded[name] = nil
  package.preload[name] = function()
    body_count = body_count + 1
    assert(entered:send(body_count, 10) == true)
    assert(recv_expect(release, 10, "module release") == "go")
    return { marker = "loaded", count = body_count }
  end

  for i = 1, workers_n do
    threads[i] = th.spawn(function(modname, ready_ch, start_ch)
      assert(ready_ch:send("ready", 10) == true)
      assert(start_ch:recv(10) == "go")
      local mod = require(modname)
      return mod.marker, mod.count
    end, name, ready, start)
  end

  wait_ready(ready, workers_n)
  release_all(start, workers_n)
  assert(recv_expect(entered, 10, "module body entry") == 1)
  th.sleep(0.05)
  local extra, extra_ok = entered:recv(0)
  release_all(release, workers_n)
  local joined = join_all(threads)

  assert(extra == nil and extra_ok == "timeout", "duplicate module body entry")
  assert(body_count == 1, "module body ran more than once")
  for i = 1, #joined do
    assert(joined[i][2] == "loaded")
    assert(joined[i][3] == 1)
  end
  package.preload[name] = nil
  package.loaded[name] = nil
end

local function concurrent_error_releases_waiters()
  local name = "threading_require_once_error"
  local ready = th.channel(workers_n)
  local start = th.channel(workers_n)
  local entered = th.channel(workers_n)
  local release = th.channel(workers_n)
  local body_count = 0
  local threads = {}

  package.loaded[name] = nil
  package.preload[name] = function()
    body_count = body_count + 1
    assert(entered:send(body_count, 10) == true)
    assert(recv_expect(release, 10, "error module release") == "go")
    error("require_error_boom", 0)
  end

  for i = 1, workers_n do
    threads[i] = th.spawn(function(modname, ready_ch, start_ch)
      assert(ready_ch:send("ready", 10) == true)
      assert(start_ch:recv(10) == "go")
      local ok, err = pcall(require, modname)
      return ok, tostring(err)
    end, name, ready, start)
  end

  wait_ready(ready, workers_n)
  release_all(start, workers_n)
  assert(recv_expect(entered, 10, "error module body entry") == 1)
  th.sleep(0.05)
  local extra, extra_ok = entered:recv(0)
  release_all(release, workers_n)
  local joined = join_all(threads)
  local original_errors = 0
  local previous_errors = 0

  assert(extra == nil and extra_ok == "timeout", "duplicate error body entry")
  assert(body_count == 1, "error module body ran more than once")
  for i = 1, #joined do
    local ok, err = joined[i][2], joined[i][3]
    assert(ok == false)
    if err:match("require_error_boom") then
      original_errors = original_errors + 1
    elseif err:match("loop or previous error loading module") then
      previous_errors = previous_errors + 1
    else
      error("unexpected require error: " .. err)
    end
  end
  assert(original_errors == 1)
  assert(previous_errors == workers_n - 1)
  package.preload[name] = nil
  package.loaded[name] = nil
end

local function stock_false_reload()
  local name = "threading_require_false_reload"
  local loads = 0
  package.loaded[name] = nil
  package.preload[name] = function()
    loads = loads + 1
    if loads == 1 then
      package.loaded[name] = false
      return nil
    end
    return loads
  end
  assert(require(name) == false)
  assert(require(name) == 2)
  package.preload[name] = nil
  package.loaded[name] = nil
end

local function stock_module_recursion_errors()
  local name = "threading_require_recursive_sentinel"
  package.loaded[name] = nil
  package.preload[name] = function()
    local ok, err = pcall(require, name)
    assert(ok == false)
    assert(tostring(err):match("loop or previous error loading module"))
    return true
  end
  assert(require(name) == true)
  package.preload[name] = nil
  package.loaded[name] = nil
end

local function stock_loader_recursion_before_sentinel()
  local name = "threading_require_loader_reentry"
  local depth = 0
  local loader
  package.loaded[name] = nil
  loader = function(modname)
    if modname ~= name then
      return "\n\tno test loader"
    end
    depth = depth + 1
    if depth == 1 then
      local ok, value = pcall(require, name)
      assert(ok == true and value == "inner")
      return function() return "outer" end
    end
    return function() return "inner" end
  end
  table.insert(package.loaders, 1, loader)
  local ok, value = pcall(require, name)
  table.remove(package.loaders, 1)
  assert(ok == true, tostring(value))
  assert(value == "outer")
  assert(package.loaded[name] == "outer")
  package.loaded[name] = nil
end

concurrent_success_once()
concurrent_error_releases_waiters()
stock_false_reload()
stock_module_recursion_errors()
stock_loader_recursion_before_sentinel()

print("t-threading-require-once OK: require claims serialize peer module loads")
