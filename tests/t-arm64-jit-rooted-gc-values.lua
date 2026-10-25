-- Vary GC live ranges independently of the helper graph: loop-carried
-- results, identity keys, SLOAD sources and many simultaneous spilled values.
local jit, util, bit = require('jit'), require('jit.util'), require('bit')
local vmdef, ffi = require('jit.vmdef'), require('ffi')
local threading = require('threading')
assert(jit.arch == 'arm64' and jit.status())
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
  return true
end, ready, release)
assert(select(2, ready:recv(5)))

local values = {
  {'table', {}}, {'string', string.rep('gc-value', 9)},
  {'function', function() return 42 end},
  {'thread', coroutine.create(function() end)},
  {'userdata', newproxy(true)}, {'cdata', ffi.new('int[1]', 7)},
}

local function run(name, fn, args, expected, minimum_reads, minimum_stores)
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4', 'maxrecord=4000')
  local roots, aborts, exits = {}, {}, 0
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == fn then roots[no] = true end
    if what == 'abort' and roots[no] then
      roots[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function exit(no) if roots[no] then exits = exits+1 end end
  jit.attach(trace, 'trace'); jit.attach(exit, 'texit')
  for _ = 1, 20 do assert(fn(unpack(args)) == expected, name..': result identity') end
  jit.attach(trace); jit.attach(exit)
  local count, reads, stores = 0, 0, 0
  for no in pairs(roots) do
    local info = util.traceinfo(no)
    if info then
      count = count+1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local idx = bit.rshift(op, 8)
          local opname = vmdef.irnames:sub(idx*6+1, idx*6+6)
          assert(opname ~= 'ALOAD ' and opname ~= 'HLOAD ' and
                 opname ~= 'ASTORE' and opname ~= 'HSTORE' and
                 opname ~= 'FSTORE', name)
          if opname == 'CALLS ' then
            if vmdef.ircall[id] == 'lj_tab_gettv_rooted_hit_try' then reads=reads+1 end
            if vmdef.ircall[id] == 'lj_tab_storegc_rooted' then stores=stores+1 end
          end
        end
      end
    end
  end
  assert(count == 1 and #aborts == 0, name..': '..table.concat(aborts, ','))
  assert(reads >= minimum_reads and stores >= minimum_stores, name..': helper graph')
  assert(exits >= 19 and exits <= 21, name..': repeated exits '..exits)
  collectgarbage('collect')
  assert(fn(unpack(args)) == expected, name..': restored GC result')
  print(string.format('rooted GC values: %s reads=%d stores=%d exits=%d',
                      name, reads, stores, exits))
end
jit.off(run, true)

local function returned(source, noise, key, n)
  local value, i = source[key], 0
  while i < n do
    value = source[key]
    i = i + noise[1]
  end
  return value
end
local function identity_key(t, key, value, n)
  local last, i = value, 0
  while i < n do
    t[key] = value
    last = t[key]
    i = i+1
  end
  return last
end
local function chain(head, n)
  local current, i = head, 0
  while i < n do current = current.next; i = i+1 end
  return current
end

local source, dest, code = {}, {}, {'return function(source, dest, n)'}
code[#code+1] = 'local i = 0; while i < n do'
for i = 1, 18 do
  source[i], dest[i] = values[(i-1)%#values+1][2], false
  code[#code+1] = ('local v%d = source[%d]'):format(i, i)
end
for i = 1, 18 do code[#code+1] = ('dest[%d] = v%d'):format(i, i) end
code[#code+1] = 'i=i+1; end; return dest; end'
local pressure = assert(loadstring(table.concat(code, '\n')))()

if rooted_gc_admission then
  rooted_gc_trace_clone = assert(loadstring(string.dump(pressure)))
  run('18 live GC values', pressure, {source, dest, 32}, dest, 18, 18)
else
  for _, pair in ipairs(values) do
    local name, value = pair[1], pair[2]
    run(name..' PHI/return', returned, {{field=value}, {1}, 'field', 32}, value, 1, 0)
    local t = {[value]=value}
    run(name..' identity key/SLOAD', identity_key, {t, value, value, 32}, value, 1, 1)
    assert(t[value] == value)
  end
  local a, b, c = {}, {}, {}
  a.next, b.next, c.next = b, c, a
  run('table chain PHI', chain, {a, 33}, a, 1, 0)
  run('18 live GC values', pressure, {source, dest, 32}, dest, 18, 18)
  for i=1,18 do assert(dest[i] == source[i], 'spilled GC value '..i) end
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-gc-values OK')
