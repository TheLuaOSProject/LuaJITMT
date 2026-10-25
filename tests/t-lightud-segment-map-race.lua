-- Concurrent segment insertion and decode using supported
-- threading TGs. C callbacks only push/decode opaque pointer tokens.
local th = require 'threading'
local jit = require 'jit'
jit.off()

local prior = {}
for group=1,4 do
  prior[group] = lightud_roundtrip(group, 0x1000)
  assert(lightud_matches(prior[group], group, 0x1000))
end
local ready, start, done = th.channel(5), th.channel(5), th.channel(4)
local workers = {}
lightud_rendezvous_arm(5, 4)
for id=1,5 do
  workers[id] = th.spawn(function(id, ready_ch, start_ch, done_ch, old)
    assert(ready_ch:send(id, 10) == true)
    assert(start_ch:recv(10) == 'go')
    if id == 1 then
      -- Keep decoding established entries until every writer has finished
      -- publishing new immutable map slots.
      local writers_left = 4
      while writers_left > 0 do
        for group=1,4 do
          assert(lightud_matches(old[group], group, 0x1000),
                 'prior token changed while map grew')
        end
        local _, ok = done_ch:recv(0.001)
        if ok == true then writers_left = writers_left - 1 end
      end
      return 'reader'
    end
    local common = lightud_roundtrip(5, 0x1777)
    assert(lightud_matches(common, 5, 0x1777))
    local first = 8 + (id-2)*50 + 1
    local tokens = {}
    for i=0,49 do
      local group = first + i
      local low = 0x2000 + i*8
      local token = lightud_roundtrip(group, low)
      tokens[i+1] = token
      assert(lightud_matches(token, group, low))
    end
    assert(done_ch:send('done', 10) == true)
    return common, first, tokens
  end, id, ready, start, done, prior)
end
local seen = {}
for _=1,5 do
  local ready_id, ok = ready:recv(10)
  assert(ok == true and ready_id >= 1 and ready_id <= 5 and not seen[ready_id])
  seen[ready_id] = true
end
for _=1,5 do assert(start:send('go', 10) == true) end
local arrived, rendezvous_timeout, different_slots = lightud_rendezvous_wait(5000)
lightud_rendezvous_release()
assert(not rendezvous_timeout and not different_slots and arrived == 4,
       'same-prefix writers did not rendezvous before CAS: '..tostring(arrived))
local common
for id=1,5 do
  local ok, a, b, tokens = workers[id]:join(30)
  assert(ok == true, tostring(a))
  if id == 1 then
    assert(a == 'reader')
  else
    if common then assert(a == common, 'equal upper prefixes diverged') end
    common = a
    local first = b
    for i=0,49 do
      local group, low = first+i, 0x2000+i*8
      assert(lightud_matches(tokens[i+1],group,low), 'published token changed')
      assert(lightud_roundtrip(group,low) == tokens[i+1], 'prefix mapping moved')
      assert(lightud_matches(lightud_roundtrip(group,low),group,low))
    end
  end
end
print('t-lightud-segment-map-race concurrent phase OK')
