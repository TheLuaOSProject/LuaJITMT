local th, buffer = require('threading'), require('string.buffer')
local nworkers, pool, waves, batches, count =
  tonumber(arg[1]), tonumber(arg[2]), tonumber(arg[3]),
  tonumber(arg[4]), tonumber(arg[5])
local sentinel_mode, retain_waves, checkpoint_wave =
  tonumber(arg[6]) == 1, tonumber(arg[7]), tonumber(arg[8])
local jit_mode = jit.status() and 'on' or 'off'
assert(nworkers and nworkers >= 1 and nworkers <= 4)
assert(pool == 0 or pool == 2)
assert(th.gcworkers(pool) == 0)
local payload = ('buffer-payload\0'):rep(69):sub(1, 1024)
assert(#payload == 1024)
local stats_keys = {}
for k, v in pairs(th.gcstats()) do
  if type(v) == 'number' then stats_keys[#stats_keys + 1] = k end
end
table.sort(stats_keys)
local columns = {'scenario', 'jit', 'workers', 'gc_workers', 'wave', 'stage',
  'batch', 'total_ops', 'live_workers', 'retained_handles', 'lua_bytes',
  'elapsed_wall'}
for _, k in ipairs(stats_keys) do columns[#columns + 1] = 'gc_' .. k end
io.write(table.concat(columns, ','), '\n'); io.flush()
local start, total_ops, retained_count = th.now(), 0, 0
local function emit(wave, stage, batch, live)
  local stats = th.gcstats()
  local row = {sentinel_mode and 'sentinel' or 'ordinary', jit_mode,
    nworkers, pool, wave, stage, batch, total_ops, live, retained_count,
    string.format('%.0f', collectgarbage('count') * 1024),
    string.format('%.9f', th.now() - start)}
  for _, k in ipairs(stats_keys) do
    row[#row + 1] = string.format('%.17g', stats[k])
  end
  io.write(table.concat(row, ','), '\n'); io.flush()
  assert(io.read('*l') == 'go', 'missing memory-controller acknowledgement')
end
jit.off(emit)

local function worker(id, commands, done, payload, count, batches, enabled)
  local b = buffer.new()
  if not enabled then jit.off() end
  assert(done:send(id, 30))
  for batch = 1, batches do
    local token, ok = commands:recv(30)
    assert(ok and token == batch, 'worker batch command')
    for _ = 1, count do
      b:reset():put(payload)
      assert(b:get() == payload, 'buffer returned incorrect bytes')
    end
    assert(#b == 0)
    assert(done:send(id, 30))
  end
  local token, ok = commands:recv(30)
  assert(ok and token == 0, 'worker exit command')
  return b:put(payload), count * batches
end

local function suite()
  local sentinel, sentinel_go, sentinel_ready
  if sentinel_mode then
    sentinel_go, sentinel_ready = th.channel(1), th.channel(1)
    sentinel = th.spawn(function(ready, go)
      assert(ready:send(true, 30))
      local token, ok = go:recv(110)
      assert(ok and token == true)
      return true
    end, sentinel_ready, sentinel_go)
    local token, ok = sentinel_ready:recv(30)
    assert(ok and token == true and sentinel:running())
  end
  local extra = sentinel_mode and 1 or 0
  local kept = {}
  emit(0, 'initial', 0, extra)
  for wave = 1, waves do
    local done, commands, handles = th.channel(nworkers), {}, {}
    for id = 1, nworkers do
      commands[id] = th.channel(1)
      handles[id] = th.spawn(worker, id, commands[id], done, payload,
        count, batches, jit_mode == 'on')
    end
    local function barrier()
      local seen = {}
      for _ = 1, nworkers do
        local id, ok = done:recv(30)
        assert(ok and type(id) == 'number' and id >= 1 and id <= nworkers)
        assert(not seen[id], 'duplicate worker acknowledgement')
        seen[id] = true
      end
      for id = 1, nworkers do assert(handles[id]:running()) end
      if sentinel then assert(sentinel:running()) end
    end
    barrier()
    emit(wave, 'workers_ready', 0, nworkers + extra)
    for batch = 1, batches do
      for id = 1, nworkers do assert(commands[id]:send(batch, 30)) end
      barrier()
      total_ops = total_ops + count * nworkers
      emit(wave, 'active_batch', batch, nworkers + extra)
    end
    for id = 1, nworkers do assert(commands[id]:send(0, 30)) end
    for id = 1, nworkers do
      local ok, result, finished = handles[id]:join(30)
      assert(ok == true, 'worker join failed: ' .. tostring(result))
      assert(finished == count * batches and result:tostring() == payload)
      result = nil
      assert(not handles[id]:running())
      commands[id]:close()
    end
    done:close()
    if wave <= retain_waves then
      kept[#kept + 1] = handles
      retained_count = retained_count + nworkers
      handles, commands, done, barrier = nil, nil, nil, nil
      emit(wave, 'joined_retained', batches, extra)
    else
      handles, commands, done, barrier = nil, nil, nil, nil
      emit(wave, 'joined_dropped', batches, extra)
    end
    if retain_waves > 0 and wave == retain_waves then
      -- Each completed handle intentionally owns its returned buffer until
      -- this drop. Check one retained result again before dropping all handles.
      local ok, result, finished = kept[1][1]:join(0)
      assert(ok and result:tostring() == payload and finished == count * batches)
      result = nil
      kept, retained_count = {}, 0
      emit(wave, 'retained_handles_dropped', batches, extra)
    end
    if checkpoint_wave > 0 and wave % checkpoint_wave == 0 then
      for pass = 1, 3 do
        collectgarbage('collect')
        if sentinel then assert(sentinel:running()) end
        emit(wave, 'sentinel_full_gc_' .. pass, batches, extra)
      end
    end
  end
  kept, retained_count = nil, 0
  local b = buffer.new()
  for batch = 1, 5 do
    for _ = 1, count do
      b:reset():put(payload)
      assert(b:get() == payload)
    end
    total_ops = total_ops + count
    emit(waves, 'main_automatic_churn', batch, extra)
  end
  b:free(); b = nil
  emit(waves, 'all_handles_dropped', batches, extra)
  for pass = 1, 3 do
    collectgarbage('collect')
    if sentinel then assert(sentinel:running()) end
    emit(waves, 'final_full_gc_' .. pass, batches, extra)
  end
  if sentinel then
    assert(sentinel_go:send(true, 30))
    local ok, result = sentinel:join(30)
    assert(ok and result == true)
    sentinel_go:close(); sentinel_ready:close()
    sentinel, sentinel_go, sentinel_ready = nil, nil, nil
    emit(waves, 'sentinel_dropped', batches, 0)
  end
  assert(th.gcworkers(0) == pool)
  emit(waves, 'gc_workers_stopped', batches, 0)
  for pass = 1, 3 do
    collectgarbage('collect')
    emit(waves, 'after_stop_full_gc_' .. pass, batches, 0)
  end
end
jit.off(suite)
suite()
