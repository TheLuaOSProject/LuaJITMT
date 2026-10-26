-- Native CGET/CSET through owned stack cells while another TG is active.
local jit, util = require('jit'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local ffi, threading = require('ffi'), require('threading')
assert(jit.arch == 'arm64' and jit.status())

local ready, release = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
  return true
end, ready, release))
assert(select(2, ready:recv(5)))

local function make_functions()
  local function read(v, n)
    local cell = v
    local function observe() return cell end
    local i, result = 0, cell
    while i < n do
      result = cell
      i = i + 1
    end
    return result, observe
  end
  local function read_for(v, n)
    local cell = v
    local function observe() return cell end
    local result = cell
    for _ = 1, n do result = cell end
    return result, observe
  end
  local function write(v, n)
    local cell = v
    local function observe() return cell end
    local i, result = 0, cell
    while i < n do
      cell = v
      result = cell
      i = i + 1
    end
    return result, observe
  end
  local function write_for(v, n)
    local cell = v
    local function observe() return cell end
    local result = cell
    for _ = 1, n do
      cell = v
      result = cell
    end
    return result, observe
  end
  return read, write, read_for, write_for
end

local values = {
  {'number', 42.25}, {'integer', 73}, {'negative-zero', -0},
  {'false', false}, {'true', true}, {'nil', nil},
  {'string', string.rep('rooted-local-cell-', 7)}, {'table', {marker=17}},
  {'function', function() return 23 end},
  {'thread', coroutine.create(function() end)},
  {'userdata', newproxy(true)}, {'cdata', ffi.new('int[1]', 29)}
}
if rooted_cell_lightud then
  values[#values+1] = {'lightuserdata', rooted_cell_lightud}
end

if rooted_cell_admission then
  local read, write, read_for, write_for = make_functions()
  local mode = rooted_cell_mode or 'store'
  local fn = mode == 'get' and read or mode == 'get-forl' and read_for or
             mode == 'store-forl' and write_for or write
  local value = 1.25
  if mode:match('^store%-') and mode ~= 'store-forl' then
    local kind, found = mode:sub(7), false
    for _, pair in ipairs(values) do
      if pair[1] == kind then value, found = pair[2], true end
    end
    assert(found, 'unknown admission value '..mode)
  end
  rooted_cell_trace_clone = assert(loadstring(string.dump(fn)))
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  for _ = 1, 20 do
    local result, observe = fn(value, 32)
    assert(result == value and observe() == value)
  end
  assert(release:send(true, 5))
  local ok, result = peer:join(5)
  assert(ok and result)
  print('t-arm64-jit-rooted-cells admission selector '..mode..' ready')
  return
end

local function trace_observer(fn)
  local state = {traces={}, exits=0, aborts={}}
  local function trace(what, no, func, pc, why, detail)
    if what == 'start' and func == fn then
      local ins = util.funcbc(func, pc)
      local op = ins and vmdef.bcnames:sub(bit.band(ins, 255)*6+1,
                                         bit.band(ins, 255)*6+6)
      if op == 'LOOP  ' or op == 'FORL  ' then state.traces[no] = true end
    end
    if what == 'abort' and state.traces[no] then
      state.traces[no] = nil
      state.aborts[#state.aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function texit(no)
    if state.traces[no] then state.exits = state.exits + 1 end
  end
  jit.off(trace, true); jit.off(texit, true)
  jit.attach(trace, 'trace'); jit.attach(texit, 'texit')
  state.detach = function() jit.attach(trace); jit.attach(texit) end
  return state
end

local function verify_native(name, state, store)
  local count, gets, stores = 0, 0, 0
  for no in pairs(state.traces) do
    local info = util.traceinfo(no)
    if info then
      count = count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                    bit.rshift(op, 8)*6+6) == 'CALLS ' then
          local helper = vmdef.ircall[id]
          if helper == 'lj_func_getcell_rooted_try' then gets = gets + 1 end
          if helper == 'lj_func_storecell_rooted_try' then stores = stores + 1 end
        end
      end
    end
  end
  assert(count == 1 and #state.aborts == 0,
         name..': trace admission '..table.concat(state.aborts, ','))
  assert(gets > 0 and (not store or stores > 0), name..': missing cell helpers')
  assert(state.exits >= 19 and state.exits <= 21,
         name..': native exits '..state.exits)
  print(string.format('rooted local %s gets=%d stores=%d exits=%d',
                      name, gets, stores, state.exits))
end

local function run_case(name, value, store, forl)
  local read, write, read_for, write_for = make_functions()
  local fn = store and (forl and write_for or write) or (forl and read_for or read)
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local state, previous = trace_observer(fn)
  for _ = 1, 20 do
    local result, observe = fn(value, 32)
    assert(result == value and observe() == value, name..': cell value/closure')
    if previous then
      assert(debug.upvalueid(previous, 1) ~= debug.upvalueid(observe, 1),
             name..': invocation reused old local cell')
      assert(previous() == value, name..': earlier invocation cell changed')
    end
    previous = observe
  end
  state.detach()
  verify_native(name, state, store)
  collectgarbage('collect')
  assert(previous() == value and fn(value, 8) == value, name..': GC/reentry')
  if name:match('negative%-zero') then
    assert(1/fn(value, 32) == -math.huge, name..': zero sign changed')
  end
end

jit.off(trace_observer, true)
jit.off(verify_native, true)
jit.off(run_case, true)
for _, pair in ipairs(values) do
  run_case(pair[1]..' read', pair[2], false, false)
  run_case(pair[1]..' write', pair[2], true, false)
end
run_case('number FORL read', 8.5, false, true)
run_case('number FORL write', 8.5, true, true)

-- The table read exits after a successful increment. The local cell must not
-- repeat that increment when the interpreter resumes at the changed value.
do
  local t = {}
  for i = 1, 32 do t[i] = i end
  local function hot(tablearg, n)
    local counter = 0
    local function observe() return counter end
    local i, sum = 1, 0
    while i <= n do
      counter = counter + 1
      sum = sum + tablearg[i]
      i = i + 1
    end
    return sum, observe
  end
  jit.flush()
  jit.on(hot, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local state = trace_observer(hot)
  for _ = 1, 20 do
    local sum, observe = hot(t, 32)
    assert(sum == 528 and observe() == 32)
  end
  verify_native('post-store continuation', state, true)
  local exits = state.exits
  t[16] = '16'
  local sum, observe = hot(t, 32)
  assert(sum == 528 and observe() == 32, 'CSET replayed/lost across type exit')
  assert(state.exits > exits, 'changed table value did not exit native trace')
  state.detach()
end

-- A peer closure shares the live parent local, mixing USETV on its TG with
-- native CGET/CSET in the owning frame. All observed values must be whole,
-- published identities. This does not assert atomic read-modify-write.
do
  local a, b = {}, {}
  local allowed = {[a]=true, [b]=true}
  local function hot(first, valid, n, hook)
    local cell = first
    local function observe() return cell end
    local function replace(v) cell = v end
    hook(observe, replace)
    local i, result = 0, cell
    while i < n do
      cell = first
      result = cell
      if not valid[result] then return nil end
      i = i + 1
    end
    return result, observe
  end
  local function noop() end
  jit.off(noop, true)
  jit.flush()
  jit.on(hot, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=8')
  local state = trace_observer(hot)
  for _ = 1, 20 do assert(hot(a, allowed, 32, noop) == a) end
  verify_native('concurrent warmup', state, true)
  local worker, rdy, start = nil, threading.channel(1), threading.channel(1)
  local function hook(get, set)
    worker = assert(threading.spawn(function(readych, go, read, write, first, second, valid)
      assert(readych:send(true, 5))
      assert(select(2, go:recv(5)))
      for i = 1, 20000 do
        write(i % 2 == 0 and first or second)
        assert(valid[read()], 'peer read an unknown cell identity')
      end
      return true
    end, rdy, start, get, set, a, b, allowed))
    assert(select(2, rdy:recv(5)))
    assert(start:send(true, 5))
  end
  jit.off(hook, true)
  local exits = state.exits
  local result, observe = hot(a, allowed, 200000, hook)
  local ok, completed = worker:join(20)
  assert(ok and completed, 'concurrent local-cell worker: '..tostring(completed))
  assert(allowed[result] and allowed[observe()], 'owner read unknown cell identity')
  assert(state.exits > exits, 'owner did not execute its native cell trace')
  state.detach()
  print('concurrent rooted local cells OK')
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-cells OK')
