-- Ordinary JFORL publication/reuse for direct assignment of Lua leaf returns.
-- The companion C fixture owns native-entry and child-exit claims.

local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local ffi = require('ffi')
assert(jit.status(), 'JIT must be enabled')
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
        if ti and ti.linktype == 'loop' and ti.link == id then return id end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local function install(shape)
  local leaf = assert(loadstring('return function('..shape.params..') '..
    shape.body..' end', '@return-candidate-'..shape.name))()
  local oracle = assert(loadstring('return function('..shape.params..') '..
    shape.body..' end', '@return-oracle-'..shape.name))()
  local capture = shape.captured and
    'local capture=function() return value end; ' or ''
  local runner_source = 'return function(fn,payload,initial,first,last) '..
    'local value=initial; '..capture..
    'for i=first,last do value=fn('..shape.args..') end; return value end'
  local run = assert(loadstring(runner_source,
    '@return-run-'..shape.name))()
  local oracle_run = assert(loadstring(runner_source,
    '@return-oracle-run-'..shape.name))()
  assert(leaf ~= oracle and run ~= oracle_run,
         shape.name..': candidate and oracle must be distinct')
  jit.off(oracle, true)
  jit.off(oracle_run, true)
  return leaf, run, oracle, oracle_run
end
jit.off(install, true)

local function same_result(got, expected)
  if type(expected) == 'table' or type(expected) == 'function' or
     type(expected) == 'thread' or type(expected) == 'userdata' then
    return rawequal(got, expected)
  end
  if type(expected) == 'cdata' then return got == expected end
  return got == expected
end
jit.off(same_result, true)

local function checked(run, leaf, oracle, oracle_run, payload, initial,
                      trace, first, last, label)
  local expected = oracle_run(oracle, payload, initial, first, last)
  local got = run(leaf, payload, initial, first, last)
  assert(same_result(got, expected),
         string.format('%s result mismatch [%d,%d]', label, first, last))
  assert(loop_trace(run) == trace,
         string.format('%s changed loop trace [%d,%d]', label, first, last))
end
jit.off(checked, true)

local function run_shape(shape)
  local leaf, run, oracle, oracle_run = install(shape)
  local payload = shape.payload
  jit.flush()
  jit.on()
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')

  local trace, attempts
  for attempt=1,8 do
    attempts = attempt
    local expected = oracle_run(oracle, shape.payload, shape.initial, 1, 96)
    local got = run(leaf, shape.payload, shape.initial, 1, 96)
    assert(same_result(got, expected),
           shape.name..': cold result differs from interpreter')
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, shape.name..': no self-linked JFORL after 8 cold calls')
  if shape.name == 'comparison-nan' then payload = 0/0 end

  local reuses = 0
  checked(run,leaf,oracle,oracle_run,payload,shape.initial,trace,
          9,40,shape.name..' normal')
  reuses = reuses+1
  for _,range in ipairs(shape.ranges or {}) do
    checked(run,leaf,oracle,oracle_run,payload,shape.initial,trace,
            range[1],range[2],shape.name..' numeric range')
    reuses = reuses+1
    checked(run,leaf,oracle,oracle_run,payload,shape.initial,trace,
            9,40,shape.name..' post-range')
    reuses = reuses+1
  end
  collectgarbage('collect')
  checked(run,leaf,oracle,oracle_run,payload,shape.initial,trace,
          1,24,shape.name..' post-GC')
  reuses = reuses+1
  return attempts,#(shape.ranges or {}),reuses
end
jit.off(run_shape, true)

local function main()
  local ready, stop = threading.channel(1), threading.channel(1)
  local peer = assert(threading.spawn(function(r,s)
    assert(r:send(true,5))
    local _, ok = s:recv(60)
    assert(ok)
    return true
  end,ready,stop))
  local _, ready_ok = ready:recv(5)
  assert(ready_ok, 'MT peer did not become ready')

  local payloads = {
    {name='nil',value=nil},
    {name='false',value=false},
    {name='true',value=true},
    {name='integer',value=37},
    {name='number',value=37.5},
    {name='string',value='direct return payload'},
    {name='table',value={marker=41}},
    {name='function',value=function() return 43 end},
    {name='thread',value=coroutine.create(function() return 47 end)},
    {name='userdata',value=assert(newproxy,'userdata constructor required')(true)},
    {name='cdata',value=ffi.new('int[1]',53)},
  }
  local cases = {}
  for _,literal in ipairs({
      {name='nil',source='nil',value=nil},
      {name='false',source='false',value=false},
      {name='true',source='true',value=true}}) do
    cases[#cases+1] = {
      name='return-literal-'..literal.name, params='a',
      body='local value=1; value='..literal.source..'; return value', args='i',
      payload=nil, initial=literal.value}
  end
  cases[#cases+1] = {
    name='literal-prefix-overwrite', params='a',
    body='local flag=true; local n=a+1; a=flag; return a',
    args='i', payload=nil, initial=true}
  for _,payload in ipairs(payloads) do
    cases[#cases+1] = {
      name='return-arg-'..payload.name, params='a,b', body='return b',
      args='i,payload', payload=payload.value, initial=payload.value}
  end
  cases[#cases+1] = {
    name='extra-nil', params='a,b,c', body='return c',
    args='i,nil,payload,true', payload='extra payload', initial=nil}
  cases[#cases+1] = {
    name='extra-boolean', params='a,b,c', body='return c',
    args='i,true,payload,false', payload={marker=59}, initial=false}
  cases[#cases+1] = {
    name='missing-return', params='a,b', body='return b',
    args='i', payload=nil, initial='previous visible value'}
  cases[#cases+1] = {
    name='missing-all', params='a,b', body='return b',
    args='', payload=nil, initial={marker=61}}
  cases[#cases+1] = {
    name='return-nonlast-actual', params='a,b,c,d', body='return c',
    args='i,17,payload,true', payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='copy-return', params='a,b', body='local value=b; return value',
    args='i,payload', payload=payloads[10].value, initial=false}
  for _,payload in ipairs({payloads[1],payloads[6],payloads[11]}) do
    cases[#cases+1] = {
      name='reassigned-'..payload.name, params='a,b', body='a=b; return a',
      args='payload,i', payload=payload.value, initial=payload.value}
  end
  for _,payload in ipairs(payloads) do
    cases[#cases+1] = {
      name='captured-return-'..payload.name, params='a,b', body='return b',
      args='i,payload', payload=payload.value, initial=nil, captured=true}
  end
  cases[#cases+1] = {
    name='earlier-numeric-temp', params='a,b',
    body='local value=b*3; b=a; return value', args='payload,i',
    payload=payloads[6].value, initial=payloads[6].value}
  cases[#cases+1] = {
    name='dead-numeric-prefix', params='a,b,c',
    body='local dead=a*3; return c', args='i,i+1,payload',
    payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='dead-prefix-overwrite', params='a,b',
    body='local x=a*3; x=b; return x', args='i,payload',
    payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='dead-prefix-dependent-chain', params='a,b',
    body='local x=a*3; local y=x+2; return b', args='i,payload',
    payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='retained-constant', params='a,b',
    body='local x=7; a=b; return x', args='i,payload',
    payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='compound-return', params='a,b',
    body='local x=a*3; local y=x+2; a=b; return y', args='i,payload',
    payload=payloads[7].value, initial=nil}
  cases[#cases+1] = {
    name='numeric-add-overflow', params='a', body='return a+3', args='i',
    payload=nil, initial=nil,
    ranges={{2147483637,2147483646},{2147483644,2147483646}}}
  cases[#cases+1] = {
    name='numeric-mul-overflow', params='a', body='return a*3', args='i',
    payload=nil, initial=nil,
    ranges={{715827882,715827883},{715827881,715827883}}}
  cases[#cases+1] = {
    name='comparison-branch', params='a',
    body='local value=7; if a < 0 then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-le', params='a',
    body='local value=7; if a <= 0 then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-gt', params='a',
    body='local value=7; if a > 0 then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-ge', params='a',
    body='local value=7; if a >= 0 then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-reversed', params='a',
    body='local value=7; if 0 > a then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-positive-zero', params='a',
    body='local value=7; if a < 0.0 then value=1 else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-negative-zero', params='a,b',
    body='local value=7; if a < b then value=1 else value=2 end; return value',
    args='i,payload', payload=-0.0, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-nan', params='a,b',
    body='local value=7; if a < b then value=1 else value=2 end; return value',
    args='i/100,payload', payload=2.5, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-negative-warm', params='a',
    body='local value=7; if a < 0 then value=1 else value=2 end; return value',
    args='i-5', payload=nil, initial=nil, ranges={{10,14}}}
  cases[#cases+1] = {
    name='comparison-primitive-overwrite', params='a',
    body='local value=true; if a < 0 then value=false else value=true end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-arm-liveness', params='a',
    body='local value=7; if a < 0 then value=a else value=2 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-arm-multiop', params='a',
    body='local value=7; if a < 0 then local x=a*3; value=x+2 else local y=a+1; value=y*2-4 end; return value',
    args='i', payload=nil, initial=nil, ranges={{-4,4}}}
  cases[#cases+1] = {
    name='comparison-early-return', params='a',
    body='if a > 16 then return a+.25 else return a-.25 end',
    args='i', payload=nil, initial=nil, ranges={{9,40}}}
  cases[#cases+1] = {
    name='comparison-sequential-early-return', params='a',
    body='if a < 0 then return a*-1 end; if a > 10 then return a*.5 end; return a+.25',
    args='i', payload=nil, initial=nil, ranges={{-4,4},{10,14}}}
  cases[#cases+1] = {
    name='comparison-sequential-three', params='a',
    body='if a < 0 then return a*-1 end; local b=a+100; if b > 150 then return b*.5 end; local c=a*2; if c > 60 then return c*.25 end; return a+.25',
    args='i', payload=nil, initial=nil, ranges={{-4,-2},{60,62},{30,32}}}
  cases[#cases+1] = {
    name='comparison-sequential-four', params='a',
    body='if a < 0 then return a*-1 end; local b=a+100; if b > 150 then return b*.5 end; local c=a*2; if c > 60 then return c*.25 end; local d=a+1; if d > 11 then return d*3 end; return a+.25',
    args='i', payload=nil, initial=nil, ranges={{-4,-2},{60,62},{40,42},{15,17}}}
  local publications,ranges,reuses,cold = 0,0,0,0
  for _,shape in ipairs(cases) do
    local attempts,nrange,nreuse = run_shape(shape)
    publications=publications+1
    ranges=ranges+nrange
    reuses=reuses+nreuse
    cold=cold+attempts
    io.write(string.format(
      'lua-return case=%s published=1 cold_calls=%d numeric_ranges=%d same_trace_reuses=%d result_match=yes\n',
      shape.name,attempts,nrange,nreuse))
  end

  assert(stop:send(true,5),'MT peer stop send failed')
  local joined,result = peer:join(5)
  assert(joined and result,'MT peer did not join cleanly')
  assert(publications == #cases and #cases == 59 and ranges == 26 and
         reuses == #cases*2+2*ranges)
  io.write(string.format(
    'lua-return ordinary OK: publications=%d numeric_ranges=%d same_trace_reuses=%d cold_calls=%d; publication only, no native-execution claim\n',
    publications,ranges,reuses,cold))
end
jit.off(main,true)
main()
