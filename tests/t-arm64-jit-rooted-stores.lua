-- Existing-key store effects compose with scalar arithmetic and rooted reads.
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

local cases = {
  {'numeric key/value', function(t, u, k, n, v)
    local i = 1
    while i <= n do t[k] = v; i = i + 1 end
    return i
  end, 2, 12.5},
  {'fractional key', function(t, u, k, n, v)
    local i = 1
    while i <= n do t[k] = v; i = i + 1 end
    return i
  end, 2.5, 14.5},
  {'string key', function(t, u, k, n, v)
    local i = 1
    while i <= n do t[k] = v; i = i + 1 end
    return i
  end, 'field', 16.5},
  {'boolean key/value', function(t, u, k, n, v)
    local i = 1
    while i <= n do t[k] = v; i = i + 1 end
    return i
  end, false, true},
  {'two stores and read', function(t, u, k, n, v)
    local i, s = 1, 0.5
    while i <= n do t[k] = v; u[k] = v*2; s = s+t[k]+u[k]; i = i+1 end
    return s
  end, 'field', 2.5, 480.5},
  {'computed key/value', function(t, u, k, n, v)
    local i = 1
    while i <= n do t[i] = i+v; i = i+1 end
    return i
  end, 64, 0.5},
}

local function run(case)
  local name, fn, key, value = unpack(case)
  local t, u = {}, {}
  for i=1,64 do t[i], u[i] = 1.5, 1.5 end
  t[key], u[key] = value, value
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local traces, exits, aborts = {}, 0, {}
  local function trace(what, no, func, _, reason, detail)
    if what == 'start' and func == fn then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(reason)..':'..tostring(detail)
    end
  end
  local function exit(no) if traces[no] then exits = exits+1 end end
  jit.attach(trace, 'trace'); jit.attach(exit, 'texit')
  for i=1,20 do assert(fn(t,u,key,64,value) == (case[5] or 65), name) end
  jit.attach(trace); jit.attach(exit)
  local native, stores = 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      native = native+1
      for ref=1,info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local opcode = bit.rshift(op,8)
          local opname = vmdef.irnames:sub(opcode*6+1,opcode*6+6)
          assert(opname ~= 'ASTORE' and opname ~= 'HSTORE' and opname ~= 'FSTORE', name)
          if opname == 'CALLS ' and
             (vmdef.ircall[id] == 'lj_tab_storenum_rooted' or
              vmdef.ircall[id] == 'lj_tab_storebool_rooted') then stores=stores+1 end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
         name..': native store admission failed: '..table.concat(aborts, ','))
  assert(stores >= 2, name..': no generated store effect')
  assert(exits >= 19 and exits <= 21, name..': repeated side exits '..exits)
  assert(t[key] == (name == 'computed key/value' and 64.5 or value))
  collectgarbage('collect')
  assert(fn(t,u,key,64,value) == (case[5] or 65))
  if name == 'numeric key/value' then
    local calls = 0
    t[key] = nil
    setmetatable(t, {__newindex = function(target, k, v)
      calls = calls+1; rawset(target,k,v+1)
    end})
    assert(fn(t,u,key,1,value) == 2)
    assert(calls == 1 and t[key] == value+1, 'deleted-key metamethod exit')
    setmetatable(t, nil)
  end
  print(string.format('rooted stores: %s traces=%d stores=%d exits=%d', name,native,stores,exits))
end
jit.off(run, true)
if rooted_store_admission then
  rooted_store_trace_clone = assert(loadstring(string.dump(cases[1][2])))
  run(cases[1])
else
  for _, case in ipairs(cases) do run(case) end
end
assert(release:send(true,5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-stores OK')
