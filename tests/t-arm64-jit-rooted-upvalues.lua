-- Native closed-upvalue loads/stores while another TG is active.
local jit, util = require('jit'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local ffi, threading = require('ffi'), require('threading')
assert(jit.arch == 'arm64' and jit.status())

local ready, release, peer
if not rooted_upvalue_no_peer then
  ready, release = threading.channel(1), threading.channel(1)
  peer = assert(threading.spawn(function(rdy, rel)
    assert(rdy:send(true, 5))
    assert(select(2, rel:recv(120)))
    return true
  end, ready, release))
  assert(select(2, ready:recv(5)))
end

local function make_cell(initial)
  local value = initial
  local function read(n)
    local i, observed = 0, value
    while i < n do
      observed = value
      i = i + 1
    end
    return observed
  end
  local function read_for(n)
    local observed = value
    for _ = 1, n do observed = value end
    return observed
  end
  local function write(v, n)
    local i, observed = 0, value
    while i < n do
      value = v
      observed = value
      i = i + 1
    end
    return observed
  end
  local function write_for(v, n)
    local observed = value
    for _ = 1, n do
      value = v
      observed = value
    end
    return observed
  end
  return read, write, read_for, write_for
end

if rooted_upvalue_admission then
  local read, write, read_for, write_for = make_cell(1.25)
  local mode = rooted_upvalue_mode or 'store'
  local literals = {
    ['literal-string'] = {"'rooted-literal'", 'rooted-literal'},
    ['literal-number'] = {'73.25', 73.25},
    ['literal-integer'] = {'73', 73},
    ['literal-double-integer'] = {'73', 73},
    ['literal-zero'] = {'0', 0},
    ['literal-negzero'] = {'0', -0},
    ['literal-nil'] = {'nil'},
    ['literal-false'] = {'false', false},
    ['literal-true'] = {'true', true},
  }
  local literal = literals[mode]
  local fn = (mode == 'get' and read) or (mode == 'get-forl' and read_for) or
             (mode == 'store' and write) or write_for
  if mode == 'no-peer-string-sub-forl' or
     mode == 'no-peer-string-sub-while' then
    local sub = string.sub
    if mode == 'no-peer-string-sub-forl' then
      fn = function(s, n)
        local observed = s
        for _ = 1, n do observed = sub(s, -4, -5) end
        return observed
      end
    else
      fn = function(s, n)
        local i, observed = 0, s
        while i < n do
          observed = sub(s, -4, -5)
          i = i + 1
        end
        return observed
      end
    end
  end
  assert(mode == 'get' or mode == 'store' or mode == 'get-forl' or
         mode == 'store-forl' or mode == 'no-peer-string-sub-forl' or
         mode == 'no-peer-string-sub-while' or literal)
  if literal then
    fn = assert(loadstring('local value; return function(n) '..
      'local i=0; while i<n do value='..literal[1]..
      '; i=i+1 end; return value end'))()
  end
  if mode == 'literal-double-integer' then
    assert(rooted_uv_force_double_literal(fn, 1, 73))
  elseif mode == 'literal-negzero' then
    assert(rooted_uv_force_double_literal(fn, 1, -0))
  end
  rooted_upvalue_trace_clone = assert(loadstring(string.dump(fn)))
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  for _ = 1, 20 do
    if mode == 'no-peer-string-sub-forl' or
       mode == 'no-peer-string-sub-while' then
      assert(fn('a\0\255bcdef', 32) == '')
    elseif mode == 'get' or mode == 'get-forl' then assert(fn(32) == 1.25)
    elseif literal then assert(fn(32) == literal[2])
    else assert(fn(1.25, 32) == 1.25) end
  end
  if peer then
    assert(release:send(true, 5))
    local ok, result = peer:join(5)
    assert(ok and result)
  end
  print('t-arm64-jit-rooted-upvalues admission selectors ready')
  return
end

local function run_case(name, value, forl)
  local read, write, read_for, write_for = make_cell(value)
  if forl then read, write = read_for, write_for end
  assert(debug.upvalueid(read, 1) == debug.upvalueid(write, 1), name..': cell identity')
  local traces, exits, aborts = {}, 0, {}
  jit.flush()
  jit.on(write, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == write then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function texit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(texit, 'texit')
  for _ = 1, 20 do assert(write(value, 32) == value, name..': write result') end
  jit.attach(trace); jit.attach(texit)

  local count, gets, stores = 0, 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      count = count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                    bit.rshift(op, 8)*6+6) == 'CALLS ' then
          local name = vmdef.ircall[id]
          if name == 'lj_func_getuv_rooted_try' then gets = gets + 1 end
          if name == 'lj_func_storeuv_rooted_try' then stores = stores + 1 end
        end
      end
    end
  end
  assert(count == 1 and #aborts == 0,
         name..': trace admission '..table.concat(aborts, ','))
  assert(gets > 0 and stores > 0, name..': missing rooted upvalue calls')
  assert(exits >= 19 and exits <= 21, name..': native exits '..exits)
  collectgarbage('collect')
  assert(read(8) == value and write(value, 8) == value, name..': GC/reentry')
  print(string.format('rooted upvalue %s gets=%d stores=%d exits=%d',
                      name, gets, stores, exits))
  return read, write, traces
end

local function run_reader_case(name, read, value)
  local traces, exits, aborts = {}, 0, {}
  jit.flush()
  jit.on(read, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == read then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function texit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(texit, 'texit')
  for _ = 1, 20 do assert(read(32) == value, name..': read result') end
  jit.attach(trace); jit.attach(texit)
  local count, gets = 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      count = count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                    bit.rshift(op, 8)*6+6) == 'CALLS ' and
           vmdef.ircall[id] == 'lj_func_getuv_rooted_try' then
          gets = gets + 1
        end
      end
    end
  end
  assert(count == 1 and #aborts == 0,
         name..': read trace admission '..table.concat(aborts, ','))
  assert(gets > 0 and exits >= 19 and exits <= 21,
         name..': missing native read effect/exits '..gets..'/'..exits)
  return traces
end

-- Literal assignments use distinct USET* bytecodes. Keep the target cell at
-- upvalue slot 2 by touching marker first, and verify the bytecode operands so
-- these cases cannot silently collapse into the dynamic USETV coverage above.
local function make_literal_setter(initial, kind)
  local numeric = {USETN_INTEGER='73', USETN_DOUBLE_INTEGER='73',
                   USETN_ZERO='0', USETN_NEGZERO='0'}
  if numeric[kind] then
    return assert(loadstring('local marker, value = ...; return function(n) '..
      'local i=0; while i<n do if marker == -1 then return value end; '..
      'value='..numeric[kind]..'; i=i+1 end; return value end, '..
      'function() return marker end'))(37, initial)
  end
  local marker, value = 37, initial
  local function set_string(n)
    local i = 0
    while i < n do
      if marker == -1 then return value end
      value = 'literal-rooted-string'
      i = i + 1
    end
    return value
  end
  local function set_number(n)
    local i = 0
    while i < n do
      if marker == -1 then return value end
      value = 73.25
      i = i + 1
    end
    return value
  end
  local function set_nil(n)
    local i = 0
    while i < n do
      if marker == -1 then return value end
      value = nil
      i = i + 1
    end
    return value
  end
  local function set_false(n)
    local i = 0
    while i < n do
      if marker == -1 then return value end
      value = false
      i = i + 1
    end
    return value
  end
  local function set_true(n)
    local i = 0
    while i < n do
      if marker == -1 then return value end
      value = true
      i = i + 1
    end
    return value
  end
  local setters = {USETP=set_nil, USETS=set_string, USETN=set_number,
                   USETP_FALSE=set_false, USETP_TRUE=set_true}
  return assert(setters[kind]), function() return marker end
end

local function assert_literal_store_bytecode(fn, wanted)
  assert(debug.getupvalue(fn, 1) == 'marker' and
         debug.getupvalue(fn, 2) == 'value', wanted..': capture order')
  local found = false
  for pc = 1, 1000 do
    local ins = util.funcbc(fn, pc)
    if not ins then break end
    local op = bit.band(ins, 0xff)
    local opname = vmdef.bcnames:sub(op * 6 + 1, op * 6 + 6):gsub('%s+$', '')
    if opname == wanted then
      local a = bit.band(bit.rshift(ins, 8), 0xff)
      assert(a == 1, wanted..': expected target upvalue index 1, got '..a)
      found = true
    end
  end
  assert(found, 'missing '..wanted..' bytecode')
end

local function run_literal_store_case(kind, expected)
  local fn = make_literal_setter('initial', kind)
  if kind == 'USETN_DOUBLE_INTEGER' then
    assert(rooted_uv_force_double_literal(fn, 2, 73))
  elseif kind == 'USETN_NEGZERO' then
    assert(rooted_uv_force_double_literal(fn, 2, -0))
  end
  local bytecode = kind:match('^USETP') and 'USETP' or
                   kind:match('^USETN') and 'USETN' or kind
  assert_literal_store_bytecode(fn, bytecode)
  local traces, exits, aborts = {}, 0, {}
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == fn then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function texit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(texit, 'texit')
  for _ = 1, 20 do assert(fn(32) == expected, kind..': result') end
  jit.attach(trace); jit.attach(texit)
  local count, stores = 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      count = count + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                    bit.rshift(op, 8)*6+6) == 'CALLS ' and
           vmdef.ircall[id] == 'lj_func_storeuv_rooted_try' then
          stores = stores + 1
        end
      end
    end
  end
  assert(count == 1 and #aborts == 0,
         kind..': trace admission '..table.concat(aborts, ','))
  assert(stores > 0 and exits >= 19 and exits <= 21,
         kind..': missing native rooted store/exits '..stores..'/'..exits)
  if type(expected) == 'number' then
    if expected == 0 then
      assert(1/fn(32) == 1/expected, kind..': zero sign changed')
    end
    if rooted_uv_literal_bits then
      assert(rooted_uv_literal_bits(fn, 2),
             kind..': native literal changed exact constant representation')
    end
  end
  print(string.format('rooted upvalue literal %s stores=%d exits=%d',
                      kind, stores, exits))
end

jit.off(run_case, true)
jit.off(run_reader_case, true)
local values = {
  {'number', 42.25}, {'integer', 73}, {'negative-zero', -0},
  {'false', false}, {'true', true}, {'nil', nil},
  {'string', string.rep('rooted-upvalue-', 7)}, {'table', {marker=17}},
  {'function', function() return 23 end},
  {'thread', coroutine.create(function() end)},
  {'userdata', newproxy(true)}, {'cdata', ffi.new('int[1]', 29)}
}
if rooted_uv_lightud then
  values[#values+1] = {'lightuserdata', rooted_uv_lightud}
end

local results = {}
for i, pair in ipairs(values) do
  local read, write, traces = run_case(pair[1], pair[2])
  local read_traces = run_reader_case(pair[1], read, pair[2])
  results[i] = {read, write, pair[2], traces, read_traces}
end

run_literal_store_case('USETS', 'literal-rooted-string')
run_literal_store_case('USETN', 73.25)
run_literal_store_case('USETN_INTEGER', 73)
if rooted_uv_force_double_literal then
  run_literal_store_case('USETN_DOUBLE_INTEGER', 73)
  run_literal_store_case('USETN_NEGZERO', -0)
end
run_literal_store_case('USETN_ZERO', 0)
run_literal_store_case('USETP', nil)
run_literal_store_case('USETP_FALSE', false)
run_literal_store_case('USETP_TRUE', true)

do
  local read, write, traces = run_case('number FORL', 8.5, true)
  local read_traces = run_reader_case('number FORL', read, 8.5)
  assert(next(traces) and next(read_traces))
  assert(write(8.5, 5) == 8.5)
end

-- A later rooted table-value type guard exits after the cell increment. The
-- numeric string remains valid for interpreted arithmetic, so the exact count
-- delta detects replay or loss of the already-committed upvalue store.
do
  local counter = 0
  local function read_counter() return counter end
  local t = {}
  for i = 1, 32 do t[i] = i end
  local function hot(n)
    local i, sum = 1, 0
    while i <= n do
      counter = counter + 1
      sum = sum + t[i]
      i = i + 1
    end
    return sum
  end
  jit.flush()
  jit.on(hot, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local traces, exits, aborts = {}, 0, {}
  local function trace(what, no, func, _, why, detail)
    if what == 'start' and func == hot then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function texit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(texit, 'texit')
  for _ = 1, 20 do assert(hot(32) == 528) end
  assert(next(traces) and exits >= 19 and #aborts == 0,
         'counter/table warm trace did not execute natively: '..
         exits..' exits; '..table.concat(aborts, ','))
  local before, exits_before = read_counter(), exits
  t[16] = '16'
  assert(hot(32) == 528, 'numeric-string interpreter continuation sum')
  assert(read_counter() == before + 32,
         'upvalue increment was replayed or lost across rooted-read exit')
  assert(exits > exits_before, 'changed table value did not exit native trace')
  jit.attach(trace); jit.attach(texit)
end

-- Debug writes replace the cell's type and must invalidate the old typed trace.
do
  local read, write, traces = run_case('type-change', 42.25)
  assert(read(8) == 42.25)
  for no in pairs(traces) do
    assert(util.traceinfo(no), 'warm trace disappeared before debug write')
  end
  assert(debug.setupvalue(read, 1, 'changed-type') == 'value')
  for no in pairs(traces) do
    assert(not util.traceinfo(no), 'debug type change retained stale trace')
  end
  assert(read(8) == 'changed-type' and
         write('changed-type', 16) == 'changed-type')
end

-- Sibling upvalue replacement must make the old closure observe the joined cell.
do
  local a, aw, at = run_case('join-left', 'left')
  local b, bw = make_cell('right')
  assert(debug.upvalueid(a, 1) ~= debug.upvalueid(b, 1))
  debug.upvaluejoin(aw, 1, bw, 1)
  debug.upvaluejoin(a, 1, b, 1)
  assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
  for no in pairs(at) do assert(not util.traceinfo(no), 'join retained left trace') end
  assert(bw('joined', 8) == 'joined' and a(8) == 'joined' and
         b(8) == 'joined')
end

-- Concurrent observation only requires complete published values; this does
-- not claim an atomic read-modify-write operation across TGs.
do
  local a, b = {}, {}
  local read, write = make_cell(a)
  local allowed = {[a]=true, [b]=true}
  local traces, gettrace, settrace = {}
  jit.flush()
  jit.on(read, true); jit.on(write, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=8')
  local function trace(what, no, fn)
    if what == 'start' then traces[no] = fn end
    if what == 'abort' then traces[no] = nil end
  end
  jit.attach(trace, 'trace')
  for _ = 1, 20 do
    assert(read(32) == a and write(a, 32) == a)
  end
  jit.attach(trace)
  for no, fn in pairs(traces) do
    if util.traceinfo(no) then
      if fn == read then gettrace = no end
      if fn == write then settrace = no end
    end
  end
  assert(gettrace and settrace, 'concurrent cells need both native roots')
  -- Event attachments are shared by all TGs. One callback counts each root;
  -- each counter has exactly one worker writing it during this observation.
  local counts = {[gettrace]=0, [settrace]=0}
  local function exit(no)
    if counts[no] then counts[no] = counts[no]+1 end
  end
  jit.off(exit, true)
  jit.attach(exit, 'texit')
  local rdy, start = threading.channel(2), threading.channel(2)
  local reader = assert(threading.spawn(function(ready, go, get, values)
    assert(ready:send(true, 5))
    assert(select(2, go:recv(5)))
    for _ = 1, 400 do
      local v = get(32)
      assert(values[v], 'reader observed a torn/unknown value')
    end
    return true
  end, rdy, start, read, allowed))
  local writer = assert(threading.spawn(function(ready, go, set, first, second)
    assert(ready:send(true, 5))
    assert(select(2, go:recv(5)))
    for i = 1, 400 do set(i % 2 == 0 and first or second, 32) end
    return true
  end, rdy, start, write, a, b))
  for _ = 1, 2 do assert(select(2, rdy:recv(5))) end
  for _ = 1, 2 do assert(start:send(true, 5)) end
  local ok, result = writer:join(20)
  assert(ok and result, 'concurrent writer: '..tostring(result))
  local read_ok, read_result = reader:join(20)
  assert(read_ok and read_result, 'concurrent reader: '..tostring(read_result))
  jit.attach(exit)
  assert(counts[gettrace] > 0, 'concurrent reader never entered its native trace')
  assert(counts[settrace] > 0, 'concurrent writer never entered its native trace')
  print(string.format('concurrent rooted upvalues: reader exits=%d writer exits=%d',
                      counts[gettrace], counts[settrace]))
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-upvalues OK')
