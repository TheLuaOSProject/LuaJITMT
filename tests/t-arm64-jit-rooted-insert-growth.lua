local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')
assert(jit.arch == 'arm64' and jit.status())

local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  local _, ok = rel:recv(120)
  assert(ok)
  return true
end, ready, release)
assert(select(2, ready:recv(5)))

local function fill_loop(t, offset, value, n)
  local i = 1
  while i <= n do
    t[i + offset] = value
    i = i + 1
  end
  return i
end

local function fill_for(t, offset, value, n)
  for i = 1, n do
    t[i + offset] = value
  end
  return n + 1
end

local function fill_raw_loop(rawsetfn, t, offset, value, n)
  local i = 1
  while i <= n do
    rawsetfn(t, i + offset, value)
    i = i + 1
  end
  return i
end

local function fill_raw_for(rawsetfn, t, offset, value, n)
  for i=1,n do rawsetfn(t, i + offset, value) end
  return n + 1
end

local function run(fn, rawmode, offset, value, label)
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local active, exits, aborts = {}, 0, {}
  local function trace(what, no, target, _, reason, detail)
    if what == 'start' and target == fn then active[no] = true end
    if what == 'abort' and active[no] then
      active[no] = nil
      aborts[#aborts+1] = tostring(reason)..':'..tostring(detail)
    end
  end
  local function texit(no)
    if active[no] then exits = exits + 1 end
  end
  jit.attach(trace, 'trace')
  jit.attach(texit, 'texit')
  local tables = {}
  for round = 1, 20 do
    local t = {}
    local redirect = {}
    if rawmode then
      local method = round % 2 == 0 and redirect or
        function() error('raw growth called __newindex') end
      setmetatable(t, {__newindex = method})
    end
    tables[round] = t
    local result = rawmode and fn(rawset, t, offset, value, 256) or
                             fn(t, offset, value, 256)
    assert(result == 257)
    assert(next(redirect) == nil)
    for i = 1, 256 do assert(t[i+offset] == value) end
    if round == 10 then collectgarbage('collect') end
  end
  local native, effects = 0, 0
  for no in pairs(active) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        local opcode = op and bit.rshift(op, 8)
        local name = opcode and vmdef.irnames:sub(opcode*6+1, opcode*6+6)
        assert(name ~= 'ASTORE' and name ~= 'HSTORE' and name ~= 'FSTORE')
        if name == 'CALLS ' and vmdef.ircall[id] ==
           (rawmode and 'lj_tab_storeraw_rooted' or 'lj_tab_storenew_rooted') then
          effects = effects + 1
        end
      end
    end
  end
  assert(native == 1 and effects > 0 and #aborts == 0,
    label..': no rooted growth trace '..table.concat(aborts, ','))
  -- One loop-completion exit per call proves insertion did not fall back for
  -- every new key. This bound also permits an isolated collection exit.
  assert(exits >= 19 and exits <= 23, label..': unexpected exits '..exits)
  collectgarbage('collect')
  for _, t in ipairs(tables) do
    local count = 0
    for key, got in pairs(t) do
      assert(key > offset and key <= offset+256 and got == value)
      count = count + 1
    end
    assert(count == 256)
  end
  jit.attach(trace); jit.attach(texit)
  print(string.format('rooted insertion growth %s: 5120 fresh keys, exits=%d',
    label, exits))
end

jit.off(run, true)
for _, spec in ipairs({
  {fill_loop, false, 'LOOP'}, {fill_for, false, 'FORL'},
  {fill_raw_loop, true, 'raw LOOP'}, {fill_raw_for, true, 'raw FORL'},
}) do
  for _, offset in ipairs({0, 65536}) do
    run(spec[1], spec[2], offset, 42.5, spec[3]..'/'..offset..'/number')
    run(spec[1], spec[2], offset, {}, spec[3]..'/'..offset..'/table')
  end
end
assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-insert-growth OK')
