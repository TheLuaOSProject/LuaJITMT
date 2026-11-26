local th, buffer = require('threading'), require('string.buffer')
th.gcworkers(tonumber(os.getenv('POOL') or 0))
jit.off()
local ready, stop = th.channel(1), th.channel(1)
local sentinel = th.spawn(function()
  assert(ready:send(true, 30))
  local value, ok = stop:recv(100)
  assert(value == true and ok)
end)
assert(ready:recv(30))
local function snapshot(stage, wave)
  local c
  for _=1,1000 do c=plain_census(); if c then break end; th.sleep(0) end
  assert(c, 'census admission did not settle')
  local fields = { 'live_tgs','live_arenas','live_empty','live_blocks','live_late',
    'dead_tgs','dead_arenas','dead_empty','dead_blocks','dead_late',
    'dead_actors','dead_queued' }
  io.write(stage, ',', wave, ',', collectgarbage('count'))
  for _, k in ipairs(fields) do io.write(',', c[k]) end
  io.write('\n'); io.flush()
end
local function wave()
  local workers = {}
  for i=1,4 do
    workers[i] = th.spawn(function()
      local b, s = buffer.new(1024), ('W'):rep(1024)
      for _=1,5000 do b:reset():put(s); assert(b:get()==s) end
      return b:put(s)
    end)
  end
  for _, h in ipairs(workers) do
    local ok, b = h:join()
    assert(ok, b); assert(b:get()==('W'):rep(1024)); b:free()
  end
end
print('stage,wave,lua_kib,live_tgs,live_arenas,live_empty,live_blocks,live_late,dead_tgs,dead_arenas,dead_empty,dead_blocks,dead_late,dead_actors,dead_queued')
snapshot('start', 0)
for i=1,16 do
  wave()
  snapshot('joined', i)
  for _=1,3 do collectgarbage('collect') end
  snapshot('collected', i)
end
assert(stop:send(true, 30)); assert(sentinel:join())
sentinel, ready, stop = nil, nil, nil
th.gcworkers(0)
for _=1,5 do collectgarbage('collect') end
snapshot('released', 16)
