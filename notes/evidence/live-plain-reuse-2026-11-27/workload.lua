local th, buffer = require('threading'), require('string.buffer')
local waves = tonumber(os.getenv('WAVES')) or 48
local mode = os.getenv('WORKLOAD') or 'mixed'
th.gcworkers(tonumber(os.getenv('POOL')) or 0)
if os.getenv('JIT') ~= 'on' then jit.off() end
local inbox, outbox = th.channel(1), th.channel(1)
local peer = assert(th.spawn(function()
  repeat
    local job, ok = inbox:recv(130)
    assert(ok)
    if job == true then return end
    for i=1,#job do
      assert(job[i][1]:get() == job[i][2])
      job[i][1]:free()
    end
    assert(outbox:send(true, 10))
  until false
end))
local row, fixed = {}, {}
local sizes = {1019, 2007, 4087, 8101}
for i=1, #sizes do row[i] = ('x'):rep(sizes[i]) end
for i=1,16 do
  local payload = ('guard' .. tostring(i)):rep(211)
  fixed[i] = {buffer.new(#payload):put(payload), payload}
end
local function churn(n)
  for i=1,n do
    local item = {i, tostring(i), {answer=42}}
    assert(item[3].answer == 42)
  end
end
jit.off(churn)
plain_census('start',0)
for wave = 1, waves do
  if mode == 'mixed' then
    for phase = 1, 3 do
      local batch = {}
      for i=1, 96 do
        local p = row[(wave+phase+i)%#row+1]
        batch[i] = {buffer.new(#p):put(p), p}
      end
      if (wave + phase) % 2 == 0 then
        assert(inbox:send(batch, 10) and outbox:recv(10))
      else
        for i=1,#batch do
          assert(batch[i][1]:get() == batch[i][2]); batch[i][1]:free()
        end
      end
    end
  else
    local b = buffer.new(1024)
    for i=1,5000 do assert(b:reset():put(row[1]):get() == row[1]) end
    b:free()
  end
  for i = 1,#fixed do assert(fixed[i][1]:tostring() == fixed[i][2]) end
  churn(1024)
  assert(peer:running())
  plain_census('wave', wave)
end
for _=1,8 do churn(1024); th.sleep(0.002) end
assert(peer:running())
plain_census('settled_guarded', waves)
for i=1,#fixed do assert(fixed[i][1]:get() == fixed[i][2]); fixed[i][1]:free() end
fixed=nil
for _=1,8 do churn(1024); th.sleep(0.002) end
assert(peer:running())
plain_census('settled_unheld', waves)
assert(inbox:send(true, 10) and peer:join(10))
th.gcworkers(0)
