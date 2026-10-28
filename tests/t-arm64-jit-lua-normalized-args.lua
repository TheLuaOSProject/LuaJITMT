-- Ordinary-runtime publication checks for normalized fixed-call arguments.
-- The companion C fixture owns native-entry and restoration claims.

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
    shape.body..' end', '@normalized-candidate-'..shape.name))()
  local oracle = assert(loadstring('return function('..shape.params..') '..
    shape.body..' end', '@normalized-oracle-'..shape.name))()
  local runner_source = 'return function(fn,payload,first,last) local sum=.5; '
    ..'for i=first,last do sum=sum+fn('..shape.args..') end; return sum end'
  local run = assert(loadstring(runner_source,
    '@normalized-run-'..shape.name))()
  local oracle_run = assert(loadstring(runner_source,
    '@normalized-oracle-run-'..shape.name))()
  assert(leaf ~= oracle and run ~= oracle_run)
  jit.off(oracle, true)
  jit.off(oracle_run, true)
  return leaf, run, oracle, oracle_run
end
jit.off(install, true)

local function checked(run, leaf, oracle, oracle_run, payload,
                      trace, first, last, label)
  local expected = oracle_run(oracle, payload, first, last)
  local got = run(leaf, payload, first, last)
  assert(got == expected,
    string.format('%s result mismatch [%d,%d]', label, first, last))
  assert(loop_trace(run) == trace,
    string.format('%s changed loop trace [%d,%d]', label, first, last))
end
jit.off(checked, true)

local function run_shape(shape)
  local leaf, run, oracle, oracle_run = install(shape)
  jit.flush()
  jit.on()
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')
  local trace, attempts
  for attempt=1,8 do
    attempts = attempt
    assert(run(leaf, shape.payload, 1, 96) ==
           oracle_run(oracle, shape.payload, 1, 96),
           shape.name..': cold result differs from interpreter')
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, shape.name..': no self-linked JFORL after 8 cold calls')
  local reuses = 0
  checked(run,leaf,oracle,oracle_run,shape.payload,trace,9,40,
          shape.name..' normal')
  reuses = reuses+1
  for _,range in ipairs(shape.ranges) do
    checked(run,leaf,oracle,oracle_run,shape.payload,trace,
            range[1],range[2],shape.name..' overflow')
    reuses = reuses+1
    checked(run,leaf,oracle,oracle_run,shape.payload,trace,9,40,
            shape.name..' post-overflow')
    reuses = reuses+1
  end
  collectgarbage('collect')
  checked(run,leaf,oracle,oracle_run,shape.payload,trace,1,24,
          shape.name..' post-GC')
  reuses = reuses+1
  return attempts,#shape.ranges,reuses
end
jit.off(run_shape, true)

local function main()
  local ready,stop = threading.channel(1),threading.channel(1)
  local peer = assert(threading.spawn(function(r,s)
    assert(r:send(true,5))
    local _,ok = s:recv(60)
    assert(ok)
    return true
  end,ready,stop))
  local _,ready_ok = ready:recv(5)
  assert(ready_ok,'MT peer did not become ready')

  local payloads = {
    table = {marker=17},
    string = 'normalized argument',
    ["function"] = function() return 23 end,
    thread = coroutine.create(function() return 31 end),
    cdata = ffi.new('int[1]', 37)
  }
  payloads.userdata = assert(newproxy, 'userdata constructor required')(true)

  local sum_ranges = {{1073741823,1073741824},
                      {1073741822,1073741824}}
  local product_ranges = {{715827882,715827883},
                          {715827881,715827883}}
  local cases = {
    {name='extra-numeric',params='a,b',body='return a+b',
     args='i,i+1,i+2',ranges=sum_ranges},
    {name='extra-nil',params='a,b',body='return a+b',
     args='i,i+1,nil',ranges=sum_ranges},
    {name='extra-true',params='a,b',body='return a+b',
     args='i,i+1,true',ranges=sum_ranges},
    {name='extra-false',params='a,b',body='return a+b',
     args='i,i+1,false',ranges=sum_ranges},
    {name='missing-tail',params='a,b,c,d',body='return a*3',
     args='i,i+1',ranges=product_ranges},
    {name='missing-many',params='a,b,c,d',body='return a*3',
     args='i',ranges=product_ranges},
    {name='missing-all',params='a,b,c',body='return 7',
     args='',ranges={}},
    {name='middle-nil',params='a,b,c',body='return a+c',
     args='i,nil,i+1',ranges=sum_ranges},
    {name='middle-true',params='a,b,c',body='return a+c',
     args='i,true,i+1',ranges=sum_ranges},
    {name='middle-false',params='a,b,c',body='return a+c',
     args='i,false,i+1',ranges=sum_ranges},
    {name='reassigned-nil',params='a,b',body='a=b; return a*3',
     args='nil,i',ranges=product_ranges},
    {name='reassigned-true',params='a,b',body='a=b; return a*3',
     args='true,i',ranges=product_ranges},
    {name='reassigned-false',params='a,b',body='a=b; return a*3',
     args='false,i',ranges=product_ranges},
  }
  local payload_types = {'table','string','function','thread','userdata','cdata'}
  for _,kind in ipairs(payload_types) do
    local payload = payloads[kind]
    if payload ~= nil then
      cases[#cases+1] = {
        name='unused-'..kind, params='payload,a,b', body='return a*2+b',
        args='payload,i,i+1', payload=payload, ranges=product_ranges}
      cases[#cases+1] = {
        name='extra-'..kind, params='a,b', body='return a*2+b',
        args='i,i+1,payload', payload=payload, ranges=product_ranges}
      cases[#cases+1] = {
        name='reassigned-'..kind, params='a,b', body='a=b; return a*3',
        args='payload,i', payload=payload, ranges=product_ranges}
    end
  end

  local publications,ranges,reuses,cold = 0,0,0,0
  for _,shape in ipairs(cases) do
    shape.payload = shape.payload or false
    local attempts,nrange,nreuse = run_shape(shape)
    publications=publications+1
    ranges=ranges+nrange
    reuses=reuses+nreuse
    cold=cold+attempts
    io.write(string.format(
      'lua-normalized-args case=%s published=1 cold_calls=%d overflow_ranges=%d same_trace_reuses=%d result_match=yes\n',
      shape.name,attempts,nrange,nreuse))
  end
  assert(stop:send(true,5),'MT peer stop send failed')
  local joined,result = peer:join(5)
  assert(joined and result,'MT peer did not join cleanly')
  assert(publications == 31 and ranges == 60 and reuses == 182)
  io.write(string.format(
    'lua-normalized-args ordinary OK: publications=%d overflow_ranges=%d same_trace_reuses=%d cold_calls=%d; publication only, no native-execution claim\n',
    publications,ranges,reuses,cold))
end
jit.off(main,true)
main()
