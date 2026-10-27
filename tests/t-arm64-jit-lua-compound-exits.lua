-- Ordinary-runtime publication and interpreter-result check for compound
-- pure Lua integer leaves. Publication is not evidence of native entry.

local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
assert(jit.status(), 'JIT must be enabled')

-- Keep the harness and dynamic compilation helpers interpreted. Candidate
-- traces are enabled explicitly only after their fresh prototypes exist.
jit.off(true, true)

local function loop_trace(fn)
  local fi = util.funcinfo(fn)
  if not fi or not fi.bytecodes then return nil end
  for pc = 1, fi.bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      if vmdef.bcnames:sub(op*6+1, op*6+6) == 'JFORL ' then
        local id = bit.rshift(ins, 16)
        local ti = util.traceinfo(id)
        if ti and ti.linktype == 'loop' and ti.link == id then
          return id
        end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local function install_case(label, body)
  local leaf_chunk = assert(loadstring(
    'return function(x) '..body..' end', '@compound-leaf-'..label))
  local run_chunk = assert(loadstring(
    'return function(fn,first,last) local sum=.5; '..
    'for i=first,last do sum=sum+fn(i) end; return sum end',
    '@compound-run-'..label))
  local oracle_leaf_chunk = assert(loadstring(
    'return function(x) '..body..' end', '@compound-oracle-leaf-'..label))
  local oracle_run_chunk = assert(loadstring(
    'return function(fn,first,last) local sum=.5; '..
    'for i=first,last do sum=sum+fn(i) end; return sum end',
    '@compound-oracle-run-'..label))
  local leaf, run = leaf_chunk(), run_chunk()
  local oracle_leaf, oracle_run = oracle_leaf_chunk(), oracle_run_chunk()
  assert(leaf ~= oracle_leaf and run ~= oracle_run,
         label..': candidate and oracle prototypes must be distinct')
  jit.off(oracle_leaf, true)
  jit.off(oracle_run, true)
  return leaf, run, oracle_leaf, oracle_run
end
jit.off(install_case, true)

local function checked_call(run, leaf, oracle_run, oracle_leaf, trace,
                           first, last, label)
  local expected = oracle_run(oracle_leaf, first, last)
  local result = run(leaf, first, last)
  assert(result == expected,
         string.format('%s result mismatch for [%d,%d]', label, first, last))
  local reused = loop_trace(run)
  assert(reused == trace,
         string.format('%s lost/replaced trace %d for [%d,%d]',
                       label, trace, first, last))
  return result
end
jit.off(checked_call, true)

local function run_shape(shape)
  local leaf, run, oracle_leaf, oracle_run =
    install_case(shape.name, shape.body)
  jit.flush()
  jit.on()
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')

  local trace, attempts
  for attempt=1,8 do
    attempts = attempt
    local expected = oracle_run(oracle_leaf, 1, 96)
    assert(run(leaf, 1, 96) == expected,
           shape.name..': cold result differs from interpreter oracle')
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, shape.name..': no self-linked JFORL trace after 8 cold calls')
  local published = 1
  local reused_calls = 0

  checked_call(run, leaf, oracle_run, oracle_leaf, trace, 9, 40,
               shape.name..' normal reuse')
  reused_calls = reused_calls + 1

  for _, range in ipairs(shape.ranges) do
    checked_call(run, leaf, oracle_run, oracle_leaf, trace,
                 range[1], range[2], shape.name..' overflow-range')
    reused_calls = reused_calls + 1
    -- A modest range immediately after each overflow call must still resolve
    -- the runner's FORL to the same trace.
    checked_call(run, leaf, oracle_run, oracle_leaf, trace, 9, 40,
                 shape.name..' post-overflow reuse')
    reused_calls = reused_calls + 1
  end

  collectgarbage('collect')
  checked_call(run, leaf, oracle_run, oracle_leaf, trace, 1, 24,
               shape.name..' post-GC reuse')
  reused_calls = reused_calls + 1
  return published, reused_calls, attempts
end
jit.off(run_shape, true)

local function main()
  local ready, stop = threading.channel(1), threading.channel(1)
  local peer = assert(threading.spawn(function(r, s)
    assert(r:send(true, 5))
    local _, ok = s:recv(60)
    assert(ok)
    return true
  end, ready, stop))
  local _, ready_ok = ready:recv(5)
  assert(ready_ok, 'MT peer did not become ready')

  local shapes = {
    {name='chain-two', body='return (x+3)*2',
      ranges={{2147483644,2147483645},
              {1073741820,1073741821}, {1073741819,1073741821}}},
    {name='chain-locals',
      body='local a=x+3; local b=a*3; local c=x-5; return b+c',
      ranges={{2147483644,2147483645}, {715827879,715827880},
              {536870910,536870911}, {536870909,536870911}}},
    {name='cse', body='return (x+3)+(x+3)',
      ranges={{2147483644,2147483645},
              {1073741820,1073741821}, {1073741819,1073741821}}},
    {name='square', body='return x*x+3', ranges={{46340,46341}}},
    {name='num-tail', body='return (x+3)*2.5',
      ranges={{2147483644,2147483645}}},
    {name='kshort-local', body='local a=3; return (x+a)*4',
      ranges={{2147483644,2147483645}, {536870908,536870909}}},
    {name='nvchain', body='return 100-(x+3)*4',
      ranges={{2147483644,2147483645}, {536870908,536870909},
              {-536870891,-536870890}}},
    {name='reassign-mul', body='local a=x; a=a+3; return a*2',
      ranges={{2147483644,2147483645},
              {1073741820,1073741821}, {1073741819,1073741821}}},
    {name='repeated-writes',
      body='local a=x; a=a+3; a=a*2; a=a+5; return a',
      ranges={{2147483644,2147483645}, {1073741820,1073741821},
              {1073741818,1073741819}, {1073741817,1073741819}}},
    {name='old-local-live',
      body='local a=x; local b=a+3; a=a*3; return b+a',
      ranges={{2147483644,2147483645}, {715827882,715827883},
              {536870911,536870912}, {536870910,536870912}}},
    {name='reassign-num-tail',
      body='local a=x; a=a+3; return a*2.5',
      ranges={{2147483644,2147483645}}},
    {name='reassign-num', body='local a=x; a=a*2.5; return a+x*3',
      ranges={{715827882,715827883}, {715827881,715827883}}},
  }
  local publications, reuses, cold_calls, overflow_ranges = 0, 0, 0, 0
  for _, shape in ipairs(shapes) do
    local pub, reused, attempts = run_shape(shape)
    publications = publications + pub
    reuses = reuses + reused
    cold_calls = cold_calls + attempts
    overflow_ranges = overflow_ranges + #shape.ranges
    io.write(string.format(
      'lua-compound-child case=%s published=%d cold_calls=%d overflow_ranges=%d same_trace_reuses=%d result_match=yes\n',
      shape.name, pub, attempts, #shape.ranges, reused))
  end
  assert(publications == 12 and overflow_ranges == 31 and reuses == 86)
  assert(cold_calls >= publications and cold_calls <= publications * 8)

  assert(stop:send(true, 5), 'MT peer stop send failed')
  local joined, result = peer:join(5)
  assert(joined and result, 'MT peer did not join cleanly')
  io.write(string.format(
    'lua-compound-child ordinary OK: publications=%d overflow_ranges=%d same_trace_reuses=%d cold_calls=%d; publication only, no native-execution claim\n',
    publications, overflow_ranges, reuses, cold_calls))
end
jit.off(main, true)

main()
