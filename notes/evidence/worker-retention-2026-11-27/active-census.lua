local th, buffer = require('threading'), require('string.buffer')
th.gcworkers(tonumber(os.getenv('POOL') or 0))
if os.getenv("CENSUS_JIT") ~= "on" then jit.off() end
local ready = th.channel(1)
local spin_done = false
local sentinel = th.spawn(function()
  assert(ready:send(true, 30))
  local value = 0
  while not spin_done do
    for i=1,2000 do value = (value+i) % 1048576 end
  end
  return value
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
    'dead_actors','dead_queued','retired_pending','dead_trav','dead_reclaimed','pending','unlinked_epoch','ack','phase','cycle_leader','activation','handshake_epoch','handshake_pending','peer_poll','peer_request','peer_jit','peer_native','sweeps','jit_gate','bridge_ready','sweep_root','worker','deferred','displaced','runs','peer_vmstate' }
  io.write(stage, ',', wave, ',', collectgarbage('count'))
  for _, k in ipairs(fields) do io.write(',', c[k]) end
  io.write('\n'); io.flush()
  if stage == 'settled_sentinel_alive' and c.peer_vmstate > 0 then
    local util = require('jit.util')
    local trace = c.peer_vmstate
    for i=1,5 do
      local info = util.traceinfo(trace)
      if not info then break end
      io.stderr:write(('trace %d exits %d %s %d\n'):format(trace, info.nexit, info.linktype, info.link))
      local mcode, addr = util.tracemc(trace)
      require('jit.dis_x64').disass(mcode, addr, function(str) io.stderr:write(str) end)
      if trace == info.link then break end
      trace = info.link
    end
  end
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
print('stage,wave,lua_kib,live_tgs,live_arenas,live_empty,live_blocks,live_late,dead_tgs,dead_arenas,dead_empty,dead_blocks,dead_late,dead_actors,dead_queued,retired_pending,dead_trav,dead_reclaimed,pending,unlinked_epoch,ack,phase,cycle_leader,activation,handshake_epoch,handshake_pending,peer_poll,peer_request,peer_jit,peer_native,sweeps,jit_gate,bridge_ready,sweep_root,worker,deferred,displaced,runs,peer_vmstate')
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
spin_done = true
assert(sentinel:join(30))
sentinel, ready = nil, nil
th.gcworkers(0)
for _=1,5 do collectgarbage('collect') end
snapshot('released', waves)
