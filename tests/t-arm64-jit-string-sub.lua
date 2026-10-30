local jit, util, bit = require('jit'), require('jit.util'), require('bit')
local vmdef, threading = require('jit.vmdef'), require('threading')
jit.off(true, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  jit.off(true, true)
  assert(r:send(true, 5))
  while true do
    local message, ok = s:recv(120)
    assert(ok)
    if message == true then return true end
    assert(message == 'gc')
    for i = 1, 8 do collectgarbage('collect') end
    assert(r:send('collected', 30))
  end
end, ready, stop))
assert(select(2, ready:recv(5)))

local kinds = substring_fixture_kind and {substring_fixture_kind} or {'forl', 'while'}
local starts = substring_fixture_kind and {2} or {1, 2}
local widths = substring_fixture_kind and {3} or {1, 2, 3, 8}
for _, kind in ipairs(kinds) do
  for _, first in ipairs(starts) do
    for _, width in ipairs(widths) do
      jit.flush()
      jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')
      local loop = kind == 'forl' and 'for i=1,n do ' or
                   'local i=0;while i<n do i=i+1;'
      -- Keep the initial result tag stable so a while-loop entry can allocate
      -- before any interpreter iteration interns the same substring.
      local source = 'local sub=string.sub;return function(s,n)local r=s;' ..
        loop .. 'r=sub(s,' .. first .. ',' .. (first+width-1) ..
        ') end;return r end'
      local f = assert(loadstring(source, '@substring-native-candidate'))()
      local chunk = assert(loadstring(source, '@substring-native-oracle'))
      jit.off(chunk, true)
      local oracle = chunk()
      jit.off(oracle, true)
      jit.on(f, true)
      if substring_fixture_kind then
        substring_trace_clone = assert(loadstring(source))()
      end

      local s, root = 'a\0\255bcdefghijklmnopqrstuvwxyz'
      for i = 1, 8 do assert(f(s, 80) == oracle(s, 80)) end
      for pc = 0, util.funcinfo(f).bytecodes-1 do
        local ins = util.funcbc(f, pc)
        if ins then
          local offset = bit.band(ins, 255)*6
          local name = vmdef.bcnames:sub(offset+1, offset+6)
          if name == 'JLOOP ' or name == 'JFORL ' then
            root = bit.rshift(ins, 16)
          end
        end
      end
      assert(root, 'no native substring root ' .. kind .. ' ' .. first .. ' ' .. width)
      local info = assert(util.traceinfo(root))
      assert(info.link == root and info.linktype == 'loop')
      local snew = false
      for r = 1, info.nins do
        local _, op = util.traceir(root, r)
        if op then
          local offset = bit.rshift(op, 8)*6
          if vmdef.irnames:sub(offset+1, offset+6) == 'SNEW  ' then snew = true end
        end
      end
      assert(snew, 'no allocation in selected root')

      local code, addr, loopoffset = util.tracemc(root)
      local exits = 0
      local function exit(t)
        if t == root then exits = exits+1 end
      end
      jit.off(exit, true)
      jit.attach(exit, 'texit')
      local kept = {}
      for phase = 1, 2 do
        if phase == 2 then collectgarbage('collect') end
        local before = exits
        for i = 1, 8 do
          kept[#kept+1] = f(s, 80)
          assert(kept[#kept] == oracle(s, 80))
        end
        assert(exits > before, 'no selected root exits ' .. kind ..
          ' first=' .. first .. ' width=' .. width .. ' phase=' .. phase)
        for _, value in ipairs({'', 'x', 'xy', s:sub(1, first+width-1),
                                false, {}, 12345}) do
          local ok, actual = pcall(f, value, 80)
          local expected_ok, expected = pcall(oracle, value, 80)
          assert(ok == expected_ok and (not ok or actual == expected),
                 'cold result mismatch')
        end
        assert(f(s, 80) == oracle(s, 80))
        local c, a, l = util.tracemc(root)
        assert(c == code and a == addr and l == loopoffset)
      end

      -- Retain results while the peer requests collection. The C companion
      -- separately observes fresh native publication in the while-loop case.
      local fresh = {}
      assert(stop:send('gc', 5))
      for i = 1, 48 do
        local input = string.char(i, 93, first, (i*7)%256) .. s
        local actual, expected = f(input, 80), oracle(input, 80)
        assert(actual == expected)
        fresh[i] = {actual, expected}
      end
      assert(ready:recv(30) == 'collected')
      collectgarbage('collect')
      for _, pair in ipairs(fresh) do assert(pair[1] == pair[2]) end
      assert(f(s, 80) == oracle(s, 80))
      local finalcode, finaladdr, finalloop = util.tracemc(root)
      assert(finalcode == code and finaladdr == addr and finalloop == loopoffset)
      jit.attach(exit)
      for _, value in ipairs(kept) do assert(value == oracle(s, 80)) end
      if substring_fixture_kind then substring_trace_no = root end
      print(jit.arch, kind, first, width, 'root=' .. root, 'exits=' .. exits)
    end
  end
end
assert(stop:send(true, 5))
assert(peer:join(5))
