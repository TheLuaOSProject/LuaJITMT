-- Ordinary-runtime publication and interpreter agreement for sparse child
-- snapshots. The C companion proves native entry and child-exit restoration.
local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
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
  local params = shape.nargs == 8 and 'a,b,c,d,e,f,g,h' or
                 shape.nargs == 3 and 'a,b,c' or 'a,b'
  local leaf = assert(loadstring('return function('..params..') '..shape.body..
    ' end', '@sparse-candidate-'..shape.name))()
  local oracle = assert(loadstring('return function('..params..') '..shape.body..
    ' end', '@sparse-oracle-'..shape.name))()
  local run = assert(loadstring(shape.runner, '@sparse-run-'..shape.name))()
  local oracle_run = assert(loadstring(shape.runner,
    '@sparse-oracle-run-'..shape.name))()
  assert(leaf ~= oracle and run ~= oracle_run)
  jit.off(oracle, true)
  jit.off(oracle_run, true)
  return leaf, run, oracle, oracle_run
end
jit.off(install, true)

local function checked(run, leaf, oracle, oracle_run, trace, first, last, label)
  local expected = oracle_run(oracle, first, last)
  local got = run(leaf, first, last)
  assert(got == expected,
    string.format('%s result mismatch [%d,%d]', label, first, last))
  assert(loop_trace(run) == trace,
    string.format('%s changed the loop trace [%d,%d]', label, first, last))
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
    assert(run(leaf,1,96) == oracle_run(oracle,1,96),
      shape.name..': cold result differs from interpreter')
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, shape.name..': no self-linked JFORL after 8 cold calls')
  local reuses = 0
  checked(run,leaf,oracle,oracle_run,trace,9,40,shape.name..' normal')
  reuses = reuses+1
  for _,r in ipairs(shape.ranges) do
    checked(run,leaf,oracle,oracle_run,trace,r[1],r[2],shape.name..' overflow')
    reuses = reuses+1
    checked(run,leaf,oracle,oracle_run,trace,9,40,shape.name..' post-overflow')
    reuses = reuses+1
  end
  collectgarbage('collect')
  checked(run,leaf,oracle,oracle_run,trace,1,24,shape.name..' post-GC')
  reuses = reuses+1
  return attempts,#shape.ranges,reuses
end
jit.off(run_shape,true)

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
  local runner8 = 'return function(fn,first,last) local sum=.5; '
    ..'for i=first,last do sum=sum+fn(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7) end; '
    ..'return sum end'
  local runner3 = 'return function(fn,first,last) local sum=.5; '
    ..'for i=first,last do sum=sum+fn(i,i+1,i+2) end; return sum end'
  local runner2 = 'return function(fn,first,last) local sum=.5; '
    ..'for i=first,last do sum=sum+fn(i,1) end; return sum end'
  local cases = {
    {name='unused8',nargs=8,body='return a+h',runner=runner8,
     ranges={{1073741820,1073741821},{1073741819,1073741821}}},
    {name='unused-middle3',nargs=3,body='return a+c',runner=runner3,
     ranges={{1073741822,1073741823},{1073741821,1073741823}}},
    {name='unused-tail',nargs=3,body='return a+b',runner=runner3,
     ranges={{1073741823,1073741824},{1073741822,1073741824}}},
    {name='dead-local',nargs=2,body='local dead=b*2; dead=a; return a*3',
     runner=runner2,ranges={{715827882,715827883},{715827881,715827883}}},
    {name='later-param',nargs=2,body='local x=a*3; return x+b',
     runner=runner2,ranges={{715827882,715827883},{715827881,715827883}}},
    {name='cset-old',nargs=2,body='local x=a; x=x*3; return x+b',
     runner=runner2,ranges={{715827882,715827883},{715827881,715827883}}},
  }
  local pubs,ranges,reuses,cold = 0,0,0,0
  for _,shape in ipairs(cases) do
    local attempts,nrange,nreuse = run_shape(shape)
    pubs=pubs+1; ranges=ranges+nrange; reuses=reuses+nreuse; cold=cold+attempts
    io.write(string.format('lua-sparse-exits case=%s published=1 cold_calls=%d overflow_ranges=%d same_trace_reuses=%d result_match=yes\n',
      shape.name,attempts,nrange,nreuse))
  end
  assert(stop:send(true,5),'MT peer stop send failed')
  local joined,result=peer:join(5)
  assert(joined and result,'MT peer did not join cleanly')
  assert(pubs==6 and ranges==12 and reuses==36)
  io.write(string.format('lua-sparse-exits ordinary OK: publications=%d overflow_ranges=%d same_trace_reuses=%d cold_calls=%d; publication only, no native-execution claim\n',
    pubs,ranges,reuses,cold))
end
jit.off(main,true)
main()
