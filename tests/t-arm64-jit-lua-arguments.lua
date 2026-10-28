-- Ordinary-runtime publication checks for fixed-arity pure Lua calls.
-- This reports trace publication/reuse and interpreter agreement only.

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
        if ti and ti.linktype == 'loop' and ti.link == id then
          return id
        end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local function install_case(shape)
  local params = shape.nargs == 0 and '' or
                 shape.nargs == 2 and 'a,b' or
                 shape.nargs == 3 and 'a,b,c' or
                 shape.nargs == 4 and 'a,b,c,d' or 'a,b,c,d,e,f,g,h'
  local leaf = assert(loadstring('return function('..params..') '..shape.body..
    ' end', '@args-candidate-'..shape.name))()
  local oracle = assert(loadstring('return function('..params..') '..shape.body..
    ' end', '@args-oracle-'..shape.name))()
  local run = assert(loadstring(shape.runner, '@args-run-'..shape.name))()
  local oracle_run = assert(loadstring(shape.runner,
    '@args-oracle-run-'..shape.name))()
  assert(leaf ~= oracle and run ~= oracle_run)
  jit.off(oracle, true)
  jit.off(oracle_run, true)
  return leaf, run, oracle, oracle_run
end
jit.off(install_case, true)

local function checked(run, leaf, oracle, oracle_run, trace, first, last, label)
  local expected = oracle_run(oracle, first, last)
  local got = run(leaf, first, last)
  assert(got == expected,
    string.format('%s result mismatch [%d,%d]', label, first, last))
  assert(loop_trace(run) == trace,
    string.format('%s changed loop trace [%d,%d]', label, first, last))
end
jit.off(checked, true)

local function run_shape(shape)
  local leaf, run, oracle, oracle_run = install_case(shape)
  jit.flush()
  jit.on()
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')

  local trace, attempts
  for attempt=1,8 do
    attempts = attempt
    assert(run(leaf, 1, 96) == oracle_run(oracle, 1, 96),
           shape.name..': cold result differs from interpreter')
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, shape.name..': missing self-linked JFORL after 8 calls')
  local reuses = 0
  checked(run, leaf, oracle, oracle_run, trace, 9, 40,
          shape.name..' normal reuse')
  reuses = reuses + 1
  for _, range in ipairs(shape.ranges) do
    checked(run, leaf, oracle, oracle_run, trace, range[1], range[2],
            shape.name..' child-overflow input')
    reuses = reuses + 1
    checked(run, leaf, oracle, oracle_run, trace, 9, 40,
            shape.name..' post-overflow reuse')
    reuses = reuses + 1
  end
  collectgarbage('collect')
  checked(run, leaf, oracle, oracle_run, trace, 1, 24,
          shape.name..' post-GC reuse')
  reuses = reuses + 1
  return attempts, #shape.ranges, reuses
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
    {name='calls-twice', nargs=2, body='return a+b',
     runner='return function(fn,first,last) local sum=.5; '
       ..'for i=first,last do sum=sum+fn(i,i+1); sum=sum+fn(i+2,i+3) end; '
       ..'return sum end', ranges={}},
    {name='args0', nargs=0, body='return 7',
     runner='return function(fn,first,last) local sum=.5; '
       ..'for i=first,last do sum=sum+fn() end; return sum end', ranges={}},
    {name='args8', nargs=8, body='return a+b+c+d+e+f+g+h',
     runner='return function(fn,first,last) local sum=.5; '
       ..'for i=first,last do sum=sum+fn(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7) end; '
       ..'return sum end',
     ranges={{268435452,268435453},{268435451,268435453}}},
    {name='args2-mixed', nargs=2, body='return a+b*3',
     runner='return function(fn,first,last) local sum=.5; '
       ..'for i=first,last do sum=sum+fn(.5,i) end; return sum end',
     ranges={{715827882,715827883},{715827881,715827883}}},
    {name='args2-reordered', nargs=2, body='return b*3+a*2',
     runner='return function(fn,first,last) local sum=.5; local a,b; '
       ..'for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; '
       ..'return sum end',
     ranges={{429496728,429496729},{429496727,429496729}}},
    {name='args3-constant', nargs=3, body='return b*2+c*3+a',
     runner='return function(fn,first,last) local sum=.5; local b,c; '
       ..'for i=first,last do b=i; c=i+1; sum=sum+fn(7,b,c) end; '
       ..'return sum end',
     ranges={{429496727,429496728},{429496726,429496728}}},
    {name='args4', nargs=4, body='return d+a*2+b*3+c*4',
     runner='return function(fn,first,last) local sum=.5; local a,b,c,d; '
       ..'for i=first,last do a=i; b=i+1; c=i+2; d=i+3; '
       ..'sum=sum+fn(a,b,c,d) end; return sum end',
     ranges={{214748363,214748364},{214748362,214748364}}},
    {name='args2-invariant-first', nargs=2, body='return b*3+a*2',
     runner='return function(fn,first,last) local sum=.5; local a=first; '
       ..'for i=first,last do sum=sum+fn(a,i) end; return sum end',
     ranges={{429496729,429496730},{429496728,429496731}}},
    {name='param0-write', nargs=2, body='a=a+3; return a*2+b',
     runner='return function(fn,first,last) local sum=.5; local a,b; '
       ..'for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; '
       ..'return sum end',
     ranges={{715827880,715827881},{715827879,715827881}}},
    {name='later-param-write', nargs=2, body='b=b+3; return a+b*2',
     runner='return function(fn,first,last) local sum=.5; local a,b; '
       ..'for i=first,last do a=i; b=i+1; sum=sum+fn(a,b) end; '
       ..'return sum end',
     ranges={{715827879,715827880},{715827878,715827880}}},
    {name='caller-updated', nargs=2, body='return a*2+b*3',
     runner='return function(fn,first,last) local sum=.5; local a,b; '
       ..'for i=first,last do a=i; b=i+1; a=a+1; b=b+2; '
       ..'sum=sum+fn(a,b) end; return sum end',
     ranges={{429496727,429496728},{429496726,429496728}}},
  }

  local publications, overflow_ranges, reuses, cold_calls = 0, 0, 0, 0
  for _, shape in ipairs(shapes) do
    local attempts, nrange, nr = run_shape(shape)
    publications = publications + 1
    overflow_ranges = overflow_ranges + nrange
    reuses = reuses + nr
    cold_calls = cold_calls + attempts
    io.write(string.format(
      'lua-arguments case=%s published=1 cold_calls=%d overflow_ranges=%d same_trace_reuses=%d result_match=yes\n',
      shape.name, attempts, nrange, nr))
  end
  assert(stop:send(true, 5), 'MT peer stop send failed')
  local joined, result = peer:join(5)
  assert(joined and result, 'MT peer did not join cleanly')
  assert(publications == 11 and overflow_ranges == 18 and reuses == 58)
  io.write(string.format(
    'lua-arguments ordinary OK: publications=%d overflow_ranges=%d same_trace_reuses=%d cold_calls=%d; publication only, no native-execution claim\n',
    publications, overflow_ranges, reuses, cold_calls))
end
jit.off(main, true)

main()
