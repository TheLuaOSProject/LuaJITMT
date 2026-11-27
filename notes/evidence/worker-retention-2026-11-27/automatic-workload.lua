local th, buffer = require('threading'), require('string.buffer')
th.gcworkers(tonumber(os.getenv('POOL') or 0))
if os.getenv("CENSUS_JIT") ~= "on" then jit.off() end
local ready, stop = th.channel(1), th.channel(1)
local sentinel = th.spawn(function()
  assert(ready:send(true, 30))
  local value, ok = stop:recv(100)
  assert(value == true and ok)
end)
assert(ready:recv(30))
local waves = tonumber(os.getenv("WAVES")) or 96
local keep = {}
local expected = ('W'):rep(1024)
local function snapshot(stage, wave)
  local c
  for _=1,1000 do c=plain_census(); if c then break end; th.sleep(0) end
  assert(c, 'census admission did not settle')
  local fields = { 'live_tgs','live_arenas','live_empty','live_blocks','live_late',
    'dead_tgs','dead_arenas','dead_empty','dead_blocks','dead_late',
    'dead_actors','dead_queued','retired_pending','dead_trav','dead_reclaimed','pending','unlinked_epoch','ack' }
  io.write(stage, ',', wave, ',', collectgarbage('count'))
  for _, k in ipairs(fields) do io.write(',', c[k]) end
  io.write('\n'); io.flush()
end
local function churn()
  for i=1,1024 do
    local record = {i, tostring(i), {answer=42}}
    assert(record[3].answer == 42)
  end
end
jit.off(churn)
local function wave(KEEPING)
  local workers = {}
  for i=1,4 do
    workers[i] = th.spawn(function()
      local b, s = buffer.new(1024), ('W'):rep(1024)
      for _=1,2000 do b:reset():put(s); assert(b:get()==s) end
      return b:put(s)
    end)
  end
  for _, h in ipairs(workers) do
    local ok, b = h:join()
    assert(ok, b); assert(tostring(b)==expected)
    if #keep < 4 and KEEPING then
      keep[#keep+1] = {h, b}
    else
      assert(b:get()==expected); b:free()
    end
  end
end
print('stage,wave,lua_kib,live_tgs,live_arenas,live_empty,live_blocks,live_late,dead_tgs,dead_arenas,dead_empty,dead_blocks,dead_late,dead_actors,dead_queued,retired_pending,dead_trav,dead_reclaimed,pending,unlinked_epoch,ack')
snapshot('start', 0)
for i=1,waves do
  wave(i == 1)
  snapshot('joined', i)
  for _=1,3 do churn() end
  snapshot('collected', i)
  assert(sentinel:running())
  for _, kept in ipairs(keep) do
    local ok, b = kept[1]:join(0)
    assert(ok and b == kept[2] and tostring(b) == expected)
  end
end
for _, kept in ipairs(keep) do
  assert(kept[2]:get() == expected); kept[2]:free()
end
keep = nil
for _=1,3 do churn() end
assert(sentinel:running())
snapshot('all_dropped_sentinel_alive', waves)
for i=1,12 do th.sleep(0.002); churn() end
assert(sentinel:running())
snapshot('settled_sentinel_alive', waves)
assert(stop:send(true, 30)); assert(sentinel:join())
sentinel, ready, stop = nil, nil, nil
th.gcworkers(0)
for _=1,5 do collectgarbage('collect') end
snapshot('released', waves)
