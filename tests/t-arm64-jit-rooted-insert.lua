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

local function insert_delete_loop(t, k, v, n)
  local i = 1
  while i <= n do
    t[k] = v
    t[k] = nil
    i = i + 1
  end
  return i
end

local function insert_delete_for(t, k, v, n)
  for i=1,n do
    t[k] = v
    t[k] = nil
  end
  return n + 1
end

local function raw_insert_delete_loop(rawsetfn, t, k, v, n)
  local i = 1
  while i <= n do
    local result = rawsetfn(t, k, v)
    if result ~= t then return -1 end
    rawsetfn(t, k, nil)
    i = i + 1
  end
  return i
end

local function raw_insert_delete_for(rawsetfn, t, k, v, n)
  for i=1,n do
    local result = rawsetfn(t, k, v)
    if result ~= t then return -1 end
    rawsetfn(t, k, nil)
  end
  return n + 1
end

local opcode = rooted_insert_opcode or 'loop'
local rawmode = opcode == 'rawloop' or opcode == 'rawforl'
local isfor = opcode == 'forl' or opcode == 'rawforl'
local insert_delete = rawmode and
  (isfor and raw_insert_delete_for or raw_insert_delete_loop) or
  (isfor and insert_delete_for or insert_delete_loop)
local function invoke(t, k, v, n)
  if rawmode then return insert_delete(rawset, t, k, v, n) end
  return insert_delete(t, k, v, n)
end
jit.off(invoke, true)
local keykind = rooted_insert_keykind or 'string'
local function make_key(name)
  if keykind == 'number' then return 1 end
  if keykind == 'numberhash' then return 98273 end
  if keykind == 'boolean' then return true end
  if keykind == 'gc' then return {} end
  if keykind == 'lightuserdata' then
    return assert(rooted_insert_lightud, 'missing lightuserdata key')
  end
  return 'rooted-insert-'..name
end

local function run(value, name)
  local t, k = {}, make_key(name)
  if rooted_insert_arm then rooted_insert_arm(t, k, value) end
  jit.flush()
  jit.on(insert_delete, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local active, exits, aborts = {}, 0, {}
  local function trace(what, no, fn, _, reason, detail)
    if what == 'start' and fn == insert_delete then active[no] = true end
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
  for i=1,20 do
    assert(invoke(t, k, value, 32) == 33 and t[k] == nil)
  end
  local native, effects = 0, 0
  for no in pairs(active) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref=1,info.nins do
        local _, op, _, id = util.traceir(no, ref)
        local opcode = op and bit.rshift(op,8)
        local opname = opcode and vmdef.irnames:sub(opcode*6+1,opcode*6+6)
        assert(opname ~= 'ASTORE' and opname ~= 'HSTORE' and opname ~= 'FSTORE',
          name..': raw table store escaped rooted insertion')
        if opname == 'CALLS ' and
           vmdef.ircall[id] == (rawmode and 'lj_tab_storeraw_rooted' or
                                              'lj_tab_storenew_rooted') then
          effects = effects + 1
        end
      end
    end
  end
  assert(native == 1 and effects > 0 and #aborts == 0,
    name..': no admitted rooted insertion trace '..table.concat(aborts, ','))
  assert(exits >= 19 and exits <= 21, name..': unexpected native exits '..exits)
  assert(t[k] == nil)
  collectgarbage('collect')
  for i=1,20 do
    assert(invoke(t, k, value, 32) == 33 and t[k] == nil)
  end
  assert(exits >= 39, name..': native reentry after full GC failed')
  local calls = 0
  local exits_before_meta = exits
  if rawmode then
    setmetatable(t, { __newindex = function() error('raw insertion called __newindex') end })
    for i=1,20 do
      assert(invoke(t, k, value, 32) == 33 and rawget(t, k) == nil)
    end
    assert(calls == 0 and exits >= exits_before_meta + 19 and rawget(t, k) == nil,
      name..': raw insertion consulted __newindex or failed to reenter')
  else
    setmetatable(t, { __newindex = function(target, key, val)
      calls = calls + 1
      rawset(target, key, val)
    end })
    for i=1,20 do assert(invoke(t, k, value, 32) == 33 and t[k] == nil) end
    assert(calls == 640 and exits >= exits_before_meta + 19 and t[k] == nil,
      name..': absent-key __newindex was replayed or skipped')
  end
  setmetatable(t, nil)

  jit.attach(trace); jit.attach(texit)
  print(string.format('rooted insertion %s native=%d helper=%d exits=%d',
    name, native, effects, exits))
end

jit.off(run, true)
if rooted_insert_admission then
  local mode = rooted_insert_admission
  rooted_insert_trace_clone = assert(loadstring(string.dump(insert_delete)))
  local values = {
    number = 42.5, boolean = true, ['false'] = false,
    string = 'rooted-insert-value',
    table = {}, func = function() end,
    thread = coroutine.create(function() end),
    userdata = newproxy(true), cdata = require('ffi').new('int[1]', 73),
  }
  if rooted_insert_lightud then values.lightuserdata = rooted_insert_lightud end
  local value = values[mode]
  assert(value ~= nil, 'unknown admission value type')
  run(value, mode)
else
  local ffi = require('ffi')
  local values = {
    number = 42.5, boolean = true, ['false'] = false,
    string = 'rooted-insert-value',
    table = {}, func = function() end,
    thread = coroutine.create(function() end),
    userdata = newproxy(true), cdata = ffi.new('int[1]', 73),
  }
  if rooted_insert_lightud then values.lightuserdata = rooted_insert_lightud end
  for name, value in pairs(values) do run(value, name) end
end

assert(release:send(true,5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-insert OK')
