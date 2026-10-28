-- Sample JIT coverage using identical workloads on ARM64 and x64.
-- Usage: LUA_PATH='src/?.lua;;' src/luajit bench/jit-parity.lua [case-name]
-- Correct results alone do not prove compilation. Only exits from traces
-- patched into the selected runner count as native witnesses. An observed
-- exit proves some execution of that root, not full compilation of its body.
-- No TRACE observer is installed: it would suppress ARM64 side publication.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')
assert(jit.status(), 'JIT must be enabled')
jit.off(true, true)

local cases = {
  {'numeric-for', [[return function(n)
    local s=.5; for i=1,n do s=s+i end; return s end]]},
  {'numeric-while', [[return function(n)
    local s=.5; local i=1; while i<=n do s=s+i; i=i+1 end; return s end]]},
  {'leaf-arithmetic', [[local function f(x) return x+.25 end
    return function(n) local s=.5; for i=1,n do s=s+f(i+.5) end; return s end]]},
  {'leaf-primitive', [[local function f(x) local a=false; return a end
    return function(n) local v=true; for i=1,n do v=f(i) end; return v end]]},
  {'leaf-branch', [[local function f(x)
      if x>16 then return x+.25 else return x-.25 end end
    return function(n) local s=.5; for i=1,n do s=s+f(i+.5) end; return s end]]},
  {'nested-leaf', [[local function g(x) return x+.25 end
    local function f(x) local v=g(x); return v*.5 end
    return function(n) local s=.5; for i=1,n do s=s+f(i+.5) end; return s end]]},
  {'leaf-varargs', [[local function f(...) local x,y=...; return x+y end
    return function(n) local s=.5; for i=1,n do s=s+f(i,.25) end; return s end]]},
  {'leaf-multires', [[local function f(x) return x,.25 end
    return function(n) local s=.5; for i=1,n do
      local a,b=f(i); s=s+a+b end; return s end]]},
  {'table-read', [[return function(n)
    local t={x=.25}; local s=.5; for i=1,n do s=s+t.x end; return s end]]},
  {'table-write', [[return function(n)
    local t={x=.25}; for i=1,n do t.x=i+.25 end; return t.x end]]},
  {'table-iterator', [[return function(n)
    local t={a=1,b=2,c=3,d=4}; local s=0
    for k,v in pairs(t) do s=s+v end; return s end]]},
  {'upvalue-read', [[local x=.25
    return function(n) local s=.5; for i=1,n do s=s+x end; return s end]]},
  {'math-floor', [[local floor=math.floor
    return function(n) local s=0; for i=1,n do s=s+floor(i+.25) end; return s end]]},
  {'math-sqrt', [[local sqrt=math.sqrt
    return function(n) local s=.5; for i=1,n do s=s+sqrt(i) end; return s end]]},
  {'string-length', [[return function(n)
    local s=0; local v='abc'; for i=1,n do s=s+#v end; return s end]]},
  {'string-byte', [[local byte=string.byte
    return function(n) local s=0; for i=1,n do s=s+byte('abc',2) end; return s end]]},
  -- Keep the literal cases above for baseline comparisons. These arguments
  -- prevent the string operation itself from folding to a numeric constant.
  {'string-length-dynamic', [[return function(n,v)
    local s=0; for i=1,n do s=s+#v end; return s end]], {'abc'}},
  {'string-byte-dynamic', [[local byte=string.byte
    return function(n,v) local s=0; for i=1,n do s=s+byte(v,2) end; return s end]], {'abc'}},
  {'bit-rotate', [[local ror=require('bit').ror
    return function(n) local s=1; for i=1,n do s=ror(s,3) end; return s end]]},
  {'ffi-i64-arithmetic', [[local ffi=require('ffi')
    return function(n) local s=ffi.new('int64_t',1)
      for i=1,n do s=s+3LL end; return tonumber(s) end]]},
}

local function compile(case, suffix)
  return assert(loadstring(case[2], '@jit-parity-'..case[1]..suffix))()
end

local function root_traces(fn)
  local roots={}
  for pc=0,util.funcinfo(fn).bytecodes-1 do
    local ins=util.funcbc(fn,pc)
    if ins then
      local op=bit.band(ins,255)
      local name=vmdef.bcnames:sub(op*6+1,op*6+6)
      if name=='JFORL ' or name=='JLOOP ' or name=='JITERL' then
        local no=bit.rshift(ins,16)
        if util.traceinfo(no) then roots[no]=true end
      end
    end
  end
  return roots
end

local function same(a,b)
  return type(a)==type(b) and (a==b or
    (type(a)=='number' and a~=a and b~=b))
end

local function sample(case)
  local candidate,oracle=compile(case,''),compile(case,'-oracle')
  local inputs=case[3] or {}
  jit.off(oracle,true)
  local root_exits=0
  local function observe(no)
    -- Match while this exit is being delivered, not after subsequent GC or
    -- retirement might have reused the numeric trace slot. This observer is
    -- deliberately unsuitable for timing or throughput measurements.
    if root_traces(candidate)[no] then root_exits=root_exits+1 end
  end
  jit.off(observe,true)
  jit.flush()
  jit.opt.start('hotloop=2','hotexit=2','maxtrace=128')
  jit.attach(observe,'texit')
  jit.on()
  local checks,root_count=0,0
  local ok,err=xpcall(function()
    for iteration=1,12 do
      local n=iteration%2==0 and 64 or 48
      assert(same(candidate(n,unpack(inputs)),oracle(n,unpack(inputs))),
        case[1]..' differs from interpreter')
      checks=checks+1
    end
    collectgarbage('collect')
    assert(same(candidate(32,unpack(inputs)),oracle(32,unpack(inputs))),
      case[1]..' differs after GC')
    checks=checks+1
    for _ in pairs(root_traces(candidate)) do
      root_count=root_count+1
    end
  end,debug.traceback)
  jit.attach(observe)
  assert(ok,err)
  io.write(string.format('%s\t%s\t%d\t%d\t%d\n',
    case[1],jit.arch,checks,root_count,root_exits))
  return root_exits>0
end
jit.off(compile,true)
jit.off(root_traces,true)
jit.off(same,true)
jit.off(sample,true)

local function main()
  local ready,stop=threading.channel(1),threading.channel(1)
  local peer=assert(threading.spawn(function(r,s)
    assert(r:send(true,5)); assert(select(2,s:recv(120))); return true
  end,ready,stop))
  local count,witnessed=0,0
  local ok,err=xpcall(function()
    assert(select(2,ready:recv(5)),'MT peer did not start')
    io.write('case\tarch\tsemantic_checks\tlive_root_traces\tobserved_root_exits\n')
    for _,case in ipairs(cases) do
      if not arg[1] or arg[1]==case[1] then
        count=count+1
        if sample(case) then witnessed=witnessed+1 end
      end
    end
    assert(count>0,'unknown case: '..tostring(arg[1]))
  end,debug.traceback)
  local stopped=stop:send(true,5)
  local joined,result=peer:join(5)
  assert(ok,err)
  assert(stopped and joined and result,'MT peer did not stop and join')
  io.write(string.format('# sampled=%d root_exit_witness_rows=%d; zero exits mean no witness, not proof of no native execution\n',count,witnessed))
end
jit.off(main,true)
main()
