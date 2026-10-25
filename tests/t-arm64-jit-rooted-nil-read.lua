-- Native nil-result reads with an authoritative no-metatable proof.
local jit, util, bit = require('jit'), require('jit.util'), require('bit')
local vmdef = require('jit.vmdef')
local ffi, threading = require('ffi'), require('threading')
assert(jit.arch == 'arm64' and jit.status())

local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
  return true
end, ready, release)
assert(select(2, ready:recv(5)))

local function absent_loop(t, key, n)
  local i, count = 0, 0
  local value = t[key]
  while i < n do
    value = t[key]
    if value == nil then count = count + 1 else count = count + 100 end
    i = i + 1
  end
  return count, value
end

local function absent_forl(t, key, n)
  local count, value = 0, t[key]
  for i = 1, n do
    value = t[key]
    if value == nil then count = count + 1 else count = count + 100 end
  end
  return count, value
end

local function constant_nil_loop(t, n)
  local i, count = 0, 0
  local value = t[nil]
  while i < n do
    value = t[nil]
    if value == nil then count = count + 1 else count = count + 100 end
    i = i + 1
  end
  return count, value
end

local function constant_nan_loop(t, n)
  local i, count = 0, 0
  local value = t[0/0]
  while i < n do
    value = t[0/0]
    if value == nil then count = count + 1 else count = count + 100 end
    i = i + 1
  end
  return count, value
end

local function irname(op)
  local i = bit.rshift(op, 8)
  return vmdef.irnames:sub(i*6+1, i*6+6)
end

local function warm_absence(label, fn, args, n)
  jit.flush(); jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4', 'maxrecord=4000')
  local roots, aborts, exits = {}, {}, 0
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == fn then roots[no] = true end
    if what == 'abort' and roots[no] then
      roots[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function exit(no) if roots[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(exit, 'texit')
  for _ = 1, 20 do
    local count, value = fn(unpack(args, 1, args.n or #args))
    assert(count == n and value == nil, label..' absent result')
  end
  local trace_count, reads, nil_loads, nil_phis = 0, 0, 0, 0
  for no in pairs(roots) do
    local info = util.traceinfo(no)
    if info then
      trace_count = trace_count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local name = irname(op)
          assert(name ~= 'ALOAD ' and name ~= 'HLOAD ' and
                 name ~= 'ASTORE' and name ~= 'HSTORE' and name ~= 'FSTORE',
                 label..' raw heap IR')
          if name == 'CALLS ' and
             vmdef.ircall[id] == 'lj_tab_getnil_nometa_rooted_try' then
            reads = reads + 1
          end
          if name == 'VLOAD ' and bit.band(op, 31) == rooted_nil_irtype then
            nil_loads = nil_loads + 1
          end
          if name == 'PHI   ' and bit.band(op, 31) == rooted_nil_irtype then
            nil_phis = nil_phis + 1
          end
        end
      end
    end
  end
  assert(trace_count == 1 and #aborts == 0,
         label..' trace/abort: '..table.concat(aborts, ', '))
  assert(reads > 0 and nil_loads > 0, label..' helper/nil VLOAD missing')
  assert(nil_phis == 0, label..' unexpected nil PHI')
  assert(exits >= 19 and exits <= 21, label..' warm exits '..exits)
  collectgarbage('collect')
  assert(fn(unpack(args, 1, args.n or #args)) == n,
         label..' full-GC reentry')
  jit.attach(trace); jit.attach(exit)
  return {roots=roots, exits=exits, reads=reads, nil_loads=nil_loads}
end
jit.off(warm_absence, true)

local function check_mutations(label, fn, args, key, allow_insert)
  local t = args[1]
  local n = args[args.n]
  if allow_insert then
    t[key] = 'inserted'
    assert(fn(unpack(args, 1, args.n or #args)) == n*100,
           label..' insertion fallback')
    t[key] = nil
  end
  local calls = 0
  setmetatable(t, {__index=function(_, k)
    calls = calls + 1
    if type(key) == 'number' and key ~= key then
      assert(type(k) == 'number' and k ~= k, label..' NaN metamethod key')
    else
      assert(k == key, label..' metamethod key')
    end
    return false
  end})
  assert(fn(unpack(args, 1, args.n or #args)) == n*100 and calls == n+1,
         label..' function __index')
  if key ~= nil and not (type(key) == 'number' and key ~= key) then
    setmetatable(t, {__index={ [key] = false }})
    assert(fn(unpack(args, 1, args.n or #args)) == n*100,
           label..' table __index')
  end
  setmetatable(t, {})
  assert(fn(unpack(args, 1, args.n or #args)) == n,
         label..' empty metatable fallback')
  setmetatable(t, nil)
end
jit.off(check_mutations, true)

if rooted_nil_read_admission then
  rooted_nil_read_trace_clone = assert(loadstring(string.dump(absent_loop)))
  rooted_nil_forl_trace_clone = assert(loadstring(string.dump(absent_forl)))
  rooted_nil_key_trace_clone =
    assert(loadstring(string.dump(constant_nil_loop)))
  local key, t = {}, {}
  if rooted_nil_arg_admission then key = nil end
  local stats
  if rooted_nil_key_admission then
    stats = warm_absence('admission constant nil key', constant_nil_loop,
                         {t, 32, n=2}, 32)
  else
    local args = {t, key, 32, n=3}
    stats = warm_absence('admission SLOAD key',
                         rooted_nil_forl_admission and absent_forl or absent_loop,
                         args, 32)
  end
  assert(stats.reads > 0 and stats.nil_loads > 0)
else
  local keys = {
    {'integer', 17}, {'number', 17.25}, {'true', true}, {'false', false},
    {'string', 'not-present'}, {'lightuserdata', lightud_segmented},
    {'table', {}}, {'function', function() end},
    {'thread', coroutine.create(function() end)},
    {'userdata', newproxy(true)}, {'cdata', ffi.new('int[1]', 31)},
    {'nil', nil}, {'NaN', 0/0},
  }
  for _, pair in ipairs(keys) do
    local label, key = pair[1], pair[2]
    local t = {}
    local args = {t, key, 32, n=3}
    warm_absence('key/'..label, absent_loop, args, 32)
    check_mutations('key/'..label, absent_loop, args, key,
                    key ~= nil and not (type(key) == 'number' and key ~= key))
    warm_absence('for/key/'..label, absent_forl, args, 32)
    check_mutations('for/key/'..label, absent_forl, args, key,
                    key ~= nil and not (type(key) == 'number' and key ~= key))
  end

  local t = {}
  warm_absence('constant nil key', constant_nil_loop, {t, 32, n=2}, 32)
  check_mutations('constant nil key', constant_nil_loop, {t, 32, n=2}, nil, false)
  warm_absence('constant NaN key', constant_nan_loop, {t, 32, n=2}, 32)
  check_mutations('constant NaN key', constant_nan_loop, {t, 32, n=2}, 0/0, false)
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-nil-read OK')
