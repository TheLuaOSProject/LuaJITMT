-- Native matrix for scalar TValue results read through GC identity
-- keys. The string-key cases retain the existing scalar-helper fast path.
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

local keys = {
  {'string', 'scalar-key-string'},
  {'table', {}},
  {'function', function() return 1 end},
  {'thread', coroutine.create(function() end)},
  {'userdata', newproxy(true)},
  {'cdata', ffi.new('int[1]', 7)},
}

local function read_loop(t, key, step, n)
  local value, i = t[key], 0
  while i < n do
    value = t[key]
    i = i + step
  end
  return value
end

local function read_loaded_key(t, holder, step, n)
  local key, i = holder.key, 0
  local value = t[key]
  while i < n do
    key = holder.key
    value = t[key]
    i = i + step
  end
  return value
end

local function opname(op)
  local idx = bit.rshift(op, 8)
  return vmdef.irnames:sub(idx*6+1, idx*6+6)
end

local function run(label, fn, args, t, key, initial, changed,
                   expected_helper, expected_reads)
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4', 'maxrecord=2000')
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

  t[key] = initial
  for _ = 1, 20 do assert(fn(unpack(args)) == initial, label..' initial') end
  local count, generic, scalar = 0, 0, 0
  for no in pairs(roots) do
    local info = util.traceinfo(no)
    if info then
      count = count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local name = opname(op)
          assert(name ~= 'ALOAD ' and name ~= 'HLOAD ' and
                 name ~= 'ASTORE' and name ~= 'HSTORE' and name ~= 'FSTORE', label)
          if name == 'CALLS ' then
            local call = vmdef.ircall[id]
            if call == 'lj_tab_gettv_rooted_hit_try' then generic = generic + 1 end
            if call == 'lj_tab_getscalar_rooted_try' then scalar = scalar + 1 end
          end
        end
      end
    end
  end
  assert(count == 1 and #aborts == 0, label..' trace '..table.concat(aborts, ','))
  assert(expected_helper == 'generic' and generic >= expected_reads or
         expected_helper == 'scalar' and scalar >= expected_reads,
         label..' helper generic='..generic..' scalar='..scalar)
  assert(exits >= 19 and exits <= 21, label..' loop exits '..exits)

  -- Same-type replacement must be observed by the already compiled trace.
  t[key] = changed
  assert(fn(unpack(args)) == changed, label..' changed value')
  collectgarbage('collect')
  assert(fn(unpack(args)) == changed, label..' after GC')
  jit.attach(trace); jit.attach(exit)
  print(string.format('rooted scalar GC key: %s generic=%d scalar=%d exits=%d',
                      label, generic, scalar, exits))
end
jit.off(run, true)

if rooted_scalar_keys_admission then
  -- Clone before the only warmup so the admission fixture selects exactly
  -- this root, whose key is a table SLOAD and whose result is numeric.
  rooted_scalar_key_trace_clone = assert(loadstring(string.dump(read_loop)))
  local admission_key, admission_table = {}, {}
  admission_table[admission_key] = 37.25
  run('admission table key', read_loop,
      {admission_table, admission_key, 1, 32}, admission_table, admission_key,
      37.25, 41.5, 'generic', 1)
else
  for _, item in ipairs(keys) do
    local name, key = item[1], item[2]
    local helper = name == 'string' and 'scalar' or 'generic'
    for _, result in ipairs({
      {'integer', 17, 29}, {'number', 17.25, 29.75},
      {'true', true, true}, {'false', false, false},
    }) do
      local label, initial, changed = result[1], result[2], result[3]
      local t = {[key] = initial}
      run(name..' '..label, read_loop, {t, key, 1, 32}, t, key,
          initial, changed, helper, 1)
    end
  end

  -- Here the identity key comes from an earlier rooted VLOAD in the same
  -- trace, then feeds the generic scalar-result read as IN2.
  local key, holder, t = {}, {}, {}
  holder.key, t[key] = key, 43
  run('prior rooted VLOAD key', read_loaded_key,
      {t, holder, 1, 32}, t, key, 43, 59, 'generic', 2)
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-scalar-keys OK')
