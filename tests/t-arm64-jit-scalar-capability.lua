-- Scalar families are identified by capabilities, not exact IR arrays.
local function run(name,f,...)
  print('CASE',name)
  jit.off(f); local expected=f(...); jit.on(f)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  if name:sub(1,3)=='fma' then jit.opt.start('fma') end
  local result=f(...); assert(result==expected)
  probe_reset(); result=f(...); assert(result==expected); probe_check(); probe_lifecycle(f,expected,...)
end
jit.off(run,true)
run('mixed_intermediates',function(n,a,b)
  local i,x=0,0.5
  while i<n do local y=(x+a)*b; x=y/2.0-0.25; i=i+1 end
  return x
end,20,0.125,1.25)
run('multiple_accumulators',function(n,a,b,c)
  local i,x,y=0,0.75,0.125
  while i<n do x=(x+a)/b; y=y*c-0.25; i=i+1 end
  return x+y
end,20,0.5,1.125,1.25)
run('integer_widenings',function(n,a,b)
  local i,x,y=0,0.75,0.125
  while i<n do x=(x+a)/b; y=y+0.25; i=i+1 end
  return x+y
end,20,2,3)
run('numeric_comparison',function(x,step,limit)
  local y=0.25
  while x<limit do y=(y+0.5)/1.25; x=x+step end
  return x+y
end,0.75,0.25,8.25)

run('phi_swap',function(n,x,y)
  local i=0
  while i<n do x,y=y,x+.25; i=i+1 end
  return x+2*y
end,20,.75,1.25)
probe_phis(false)
run('phi_rotation',function(n,x,y,z)
  local i=0
  while i<n do x,y,z=y,z,x+.25; i=i+1 end
  return x+2*y+4*z
end,20,.75,1.25,2.5)
probe_phis(false)
-- The loop optimizer folds the two equal RHS values into one PHI.
run('phi_shared_rhs_folded',function(n,x,y,z)
  local i=0
  while i<n do x,y,z=z+.125,z+.125,x+y; i=i+1 end
  return x+2*y+4*z
end,20,.75,1.25,2.5)
probe_phis(true)

local function overflow(n,x)
  local i,y=0,0.75
  while i<n do x=x*3; y=(y+.5)/1.25; i=i+1 end
  return x+y
end
run('overflow_numeric_live',overflow,6,3)
probe_changed(overflow,3221225469+1.2,2,357913941)
-- A fractional value fails the checked integer input speculation.
local function baseline(n,x)
  local i,y=0,.75
  while i<n do x=x*3; y=(y+.5)/1.25; i=i+1 end
  return x+y
end
jit.off(baseline)
probe_changed(overflow,baseline(6,3.25),6,3.25)
probe_flush()
run('overflow_after_flush',overflow,6,3)

run('fma',function(n,a,b)
  local i,x=0,.75
  while i<n do x=x*a+b; i=i+1 end
  return x
end,20,.5,.25)
probe_fma()


run('fma_left_sub',function(n,a,b)
  local i,x=0,.75
  while i<n do x=x*a-b; i=i+1 end
  return x
end,20,.5,.25)
probe_fma()
run('fma_right_sub',function(n,a,b)
  local i,x=0,.75
  while i<n do x=b-x*a; i=i+1 end
  return x
end,20,.5,.25)
probe_fma()
run('numeric_ne',function(n,x,a)
  local y=.25
  while x~=n do y=(y+.5)/1.25; x=x+a end
  return y+x
end,8.25,.75,.25)
local function equal(n,x,a,b,w)
  local i=0
  while i<n do x=(x+a)/b; if x~=w then return x end; i=i+1 end
  return x
end
run('numeric_eq',equal,20,.75,.75,2.0,.75)
probe_changed(equal,.75,20,.75,.75,2.0,0/0)
local function negative(name,f,...)
  print('NEGATIVE CASE',name)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1','maxtrace=1')
  probe_reset();f(...);f(...);probe_negative()
end
jit.off(negative,true)
negative('read',function(t) local i,s=0,0 while i<100 do s=s+t.value; i=i+1 end return s end,{value=2})
negative('write',function(t) local i=0 while i<100 do t.value=i; i=i+1 end return i end,{})
negative('allocation',function() local i,t=0,{} while i<100 do t={i,t}; i=i+1 end return t end)
negative('math_call',function(x) local i=0 while i<100 do x=math.sin(x); i=i+1 end return x end,.25)

local names, source = {}, {'local i=0'}
for i=1,36 do names[i]='x'..i; source[#source+1]='local '..names[i]..'='..i..'.125' end
source[#source+1]='while i<40 do'
for i=1,36 do source[#source+1]=names[i]..'='..names[i]..'*1.0001+.125' end
source[#source+1]='i=i+1 end return '..table.concat(names,',')
negative('NUM spills',assert(loadstring(table.concat(source,'\n'))))
