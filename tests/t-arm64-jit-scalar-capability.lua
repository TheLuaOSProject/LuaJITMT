-- Scalar families are identified by capabilities, not exact IR arrays.
local function run(name,f,...)
  print('CASE',name)
  jit.off(f); local expected=f(...); jit.on(f)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  if name:sub(1,3)=='fma' then jit.opt.start('fma') end
  local result=f(...); assert(result==expected)
  probe_reset(); result=f(...); assert(result==expected); probe_check(name); probe_lifecycle(f,expected,...)
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
negative('math_multi_result',function(x) local i=0 while i<100 do x=math.frexp(x); i=i+1 end return x end,.25)

local names, source = {}, {'local i=0'}
for i=1,36 do names[i]='x'..i; source[#source+1]='local '..names[i]..'='..i..'.125' end
source[#source+1]='while i<40 do'
for i=1,36 do source[#source+1]=names[i]..'='..names[i]..'*1.0001+.125' end
source[#source+1]='i=i+1 end return '..table.concat(names,',')
run('numeric_spills',assert(loadstring(table.concat(source,'\n'))))

local threading=require('threading')
local ready,release=threading.channel(1),threading.channel(1)
local peer=assert(threading.spawn(function(rdy,rel)
  assert(rdy:send(true,5))
  assert(select(2,rel:recv(120)))
  return true
end,ready,release))
assert(select(2,ready:recv(5)))

local floor_builtin=math.floor
local function math_floor(n)
  local s=0
  for i=1,n do s=s+floor_builtin(i+.25) end
  return s
end
run('math_floor',math_floor,20)
local function math_floor_guard(n,x)
  local i,s=0,0
  while i<n do s=s+floor_builtin(i+x); i=i+1 end
  return s
end
local function run_native(name,f,...)
  print('CASE',name)
  jit.off(f); local expected=f(...); jit.on(f)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  local result=f(...); assert(result==expected)
  probe_reset(); result=f(...); assert(result==expected); probe_check(name)
end
jit.off(run_native,true)
run_native('math_floor_guard',math_floor_guard,20,.25)
probe_changed(math_floor_guard,
              math_floor_guard(6,2147483648.25),
              6,2147483648.25)
probe_changed(math_floor_guard,
              math_floor_guard(6,-2147483648.25),
              6,-2147483648.25)
-- Force a full collection after the guarded NUM-to-INT exit, then re-enter
-- the same trace with its original input to check retained roots and code.
collectgarbage('collect')
probe_reset()
assert(math_floor_guard(20,.25)==190)
probe_check('math_floor_guard')

local ceil_builtin=math.ceil
local function math_ceil(n)
  local s=0
  for i=1,n do s=s+ceil_builtin(i+.25) end
  return s
end
run('math_ceil',math_ceil,20)

local sqrt_builtin=math.sqrt
local function math_sqrt(n)
  local s=.5
  for i=1,n do s=s+sqrt_builtin(i) end
  return s
end
run('math_sqrt',math_sqrt,20)
local function same_number(a,b)
  if a~=a then return b~=b end
  if b~=b or a~=b then return false end
  if a==0 then return 1/a==1/b end
  return true
end
local function math_sqrt_special(n,x)
  local i,s=0,0
  while i<n do s=s+sqrt_builtin(x-i*.25); i=i+1 end
  return s
end
local function run_native_special(name,x,op)
  op=op or sqrt_builtin
  local function special_loop(n,value)
    local i,s=0,0
    while i<n do s=s+op(value-i*.25); i=i+1 end
    return s
  end
  jit.off(special_loop)
  local expected=special_loop(12,x)
  jit.on(special_loop)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  local result=special_loop(12,x)
  assert(same_number(result,expected),name..' interpreter mismatch')
  probe_reset()
  result=special_loop(12,x)
  assert(same_number(result,expected),name..' native mismatch')
  probe_check(name)
end
jit.off(run_native_special,true)
run_native_special('math_sqrt_negative',-1)
run_native_special('math_sqrt_infinity',1/0)
run_native_special('math_sqrt_nan',0/0)

local function numeric_tostr_for(n,x)
  local out=''
  for i=1,n do out=''..(x+i*.25) end
  return out
end
run('numeric_tostr_forl',numeric_tostr_for,20,1.125)
local function numeric_tostr_while(n,x)
  local i,out=0,''
  while i<n do out=''..(x+i*.25); i=i+1 end
  return out
end
run('numeric_tostr_while',numeric_tostr_while,20,1.125)
local function numeric_tostr_special(n,x)
  local out=''
  for i=1,n do out=''..x end
  return out
end
run('numeric_tostr_negative_zero',numeric_tostr_special,12,-0.0)
run('numeric_tostr_positive_infinity',numeric_tostr_special,12,1/0)
run('numeric_tostr_negative_infinity',numeric_tostr_special,12,-1/0)
run('numeric_tostr_nan',numeric_tostr_special,12,0/0)

local function integer_tostr_forl(n)
  local out=''
  for i=1,n do out=''..i end
  return out
end
run('integer_tostr_forl',integer_tostr_forl,20)
local function integer_tostr_boundary(n,x)
  local out=''
  for i=1,n do out=''..x end
  return out
end
run('integer_tostr_min',integer_tostr_boundary,20,-2147483648)
run('integer_tostr_max',integer_tostr_boundary,20,2147483647)
local function integer_concat_overflow(n,x)
  local out=''
  for i=1,n do out=''..(x+i) end
  return out
end
jit.off(integer_concat_overflow)
local integer_overflow_expected=integer_concat_overflow(4,2147483646)
jit.on(integer_concat_overflow)
jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1')
for i=1,4 do assert(integer_concat_overflow(4,2147483646)==integer_overflow_expected) end
print('INTEGER concat overflow interpreter/JIT oracle value='..integer_overflow_expected)

local abs_builtin, min_builtin, max_builtin = math.abs, math.min, math.max
local function math_abs_guard(n,x)
  local i,s=0,0
  while i<n do s=s+abs_builtin(x-i*.25); i=i+1 end
  return s
end
run_native('math_abs',math_abs_guard,12,-2.75)
run_native_special('math_abs_negative',-2147483648.25,abs_builtin)
run_native_special('math_abs_int_min',-2147483648,abs_builtin)
local function math_minmax_guard(n,a,b,c)
  local i,s=0,0
  while i<n do s=s+min_builtin(a+i*.25,b-i*.125,c+i*.5); i=i+1 end
  return s
end
run_native('math_min',math_minmax_guard,12,-3.25,4.5,8.75)
local function math_min_int(n,a,b,c)
  local i,s=0,0
  while i<n do s=s+min_builtin(a+i,b-i,c+2*i); i=i+1 end
  return s
end
run_native('math_min_int',math_min_int,12,-30,45,80)
local function math_min_one(n,x)
  local i,s=0,0
  while i<n do s=s+min_builtin(x+i*.25); i=i+1 end
  return s
end
run_native('math_min_one',math_min_one,12,-3.25)
local function math_max_guard(n,a,b,c)
  local i,s=0,0
  while i<n do s=s+max_builtin(a+i*.25,b-i*.125,c+i*.5); i=i+1 end
  return s
end
run_native('math_max',math_max_guard,12,-3.25,4.5,8.75)
local function math_max_int(n,a,b,c)
  local i,s=0,0
  while i<n do s=s+max_builtin(a+i,b-i,c+2*i); i=i+1 end
  return s
end
run_native('math_max_int',math_max_int,12,-30,45,80)
local function math_max_one(n,x)
  local i,s=0,0
  while i<n do s=s+max_builtin(x+i*.25); i=i+1 end
  return s
end
run_native('math_max_one',math_max_one,12,-3.25)

-- Scalar math operations preserve the VM's operand-order behavior for NaNs
-- and signed zero. Each special input still traverses a hot loop and checks
-- the JIT result against the interpreter result.
local function math_special(n, fn, a, b, c)
  local i,r=0,0
  while i<n do r=fn(a,b,c); i=i+1 end
  return r
end
local function check_math_special(name, fn, a, b, c)
  print('CASE',name)
  jit.off(math_special); local expected=math_special(12,fn,a,b,c); jit.on(math_special)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  local actual=math_special(12,fn,a,b,c)
  assert(same_number(actual,expected),name..' interpreter mismatch')
  probe_reset(); actual=math_special(12,fn,a,b,c)
  assert(same_number(actual,expected),name..' native mismatch')
  probe_check(name)
  return actual
end
jit.off(check_math_special,true)
check_math_special('math_min_nan_left',min_builtin,0/0,1,2)
check_math_special('math_min_nan_middle',min_builtin,1,0/0,2)
check_math_special('math_min_nan_right',min_builtin,1,2,0/0)
check_math_special('math_max_nan_left',max_builtin,0/0,1,2)
check_math_special('math_max_nan_middle',max_builtin,1,0/0,2)
check_math_special('math_max_nan_right',max_builtin,1,2,0/0)
check_math_special('math_min_infinities',min_builtin,-1/0,1/0,0)
check_math_special('math_max_infinities',max_builtin,-1/0,1/0,0)
check_math_special('math_min_zero_order',min_builtin,-0.0,0.0,1)
check_math_special('math_min_zero_reverse',min_builtin,0.0,-0.0,1)
local max_zero_order = check_math_special('math_max_zero_order',
                                          max_builtin,-0.0,0.0,-1)
local max_zero_reverse = check_math_special('math_max_zero_reverse',
                                            max_builtin,0.0,-0.0,-1)
assert(max_zero_order == 0 and
       1/max_zero_order == 1/max_builtin(-0.0,0.0,-1))
assert(max_zero_reverse == 0 and
       1/max_zero_reverse == 1/max_builtin(0.0,-0.0,-1))
assert(1/math_special(12,min_builtin,-0.0,0.0,1)==1/
       min_builtin(-0.0,0.0,1))
assert(1/math_special(12,min_builtin,0.0,-0.0,1)==1/
       min_builtin(0.0,-0.0,1))

local function integer_mod_constant(n,x)
  local i,s=0,.5
  while i<n do s=s+(x+i)%7; i=i+1 end
  return s
end
local function integer_mod_dynamic(n,x,d)
  local i,s=0,.5
  while i<n do s=s+(x+i)%d; i=i+1 end
  return s
end
run('integer_mod_constant',integer_mod_constant,32,8)
run('integer_mod_dynamic',integer_mod_dynamic,32,8,7)
run('integer_mod_negative',integer_mod_dynamic,32,-80,-7)

local function integer_mod_result(n,x,d)
  local i,sum,result=0,.5,0
  while i<n do result=(x+i)%d; sum=sum+result; i=i+1 end
  return sum+result
end
run('integer_mod_result_dynamic',integer_mod_result,32,8,7)

local function numeric_neg(n,x)
  local i,y=0,x
  while i<n do y=-y; i=i+1 end
  return y
end
run_native('numeric_neg',numeric_neg,13,2147483648.25)
run_native('numeric_neg_zero_positive',numeric_neg,13,0.0)
assert(1/numeric_neg(13,0.0)==-math.huge)
run_native('numeric_neg_zero_negative',numeric_neg,13,-0.0)
assert(1/numeric_neg(13,-0.0)==math.huge)

local function math_parameter(n,fn)
  local s=0
  for i=1,n do s=s+fn(i+.25) end
  return s
end
run('math_parameter',math_parameter,20,floor_builtin)
local function math_mixed(n)
  local s=.5
  for i=1,n do s=s+floor_builtin(i+.25)+sqrt_builtin(i) end
  return s
end
run('math_mixed',math_mixed,20)
local function math_four(n)
  local s=.5
  for i=1,n do
    local x=i+.25
    s=s+floor_builtin(x)+sqrt_builtin(i)+abs_builtin(-x)+min_builtin(x,2)
  end
  return s
end
run_native('math_four',math_four,20)



local function run_carried(name,op)
  local function carried(n,x)
    local i=0
    while i<n do x=op(x); i=i+1 end
    return x
  end
  jit.off(carried); local expected=carried(12,.25); jit.on(carried)
  jit.flush(); jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  assert(same_number(carried(12,.25),expected),name..' oracle mismatch')
  probe_reset()
  assert(same_number(carried(12,.25),expected),name..' native mismatch')
  probe_check('math_'..name..'_carried')
end
jit.off(run_carried,true)
for _,name in ipairs({'log','log10','exp','sin','cos','tan','asin','acos',
                       'atan','sinh','cosh','tanh'}) do
  local op=math[name]
  run_carried(name,op)
  local function helper_loop(n,x)
    local s=.25
    for i=1,n do s=s+op(x+i*.001) end
    return s
  end
  run('math_'..name,helper_loop,12,.25)
  run_native_special('math_'..name..'_nan',0/0,op)
  run_native_special('math_'..name..'_infinity',1/0,op)
end
for _,name in ipairs({'atan2','pow','ldexp'}) do
  local op=math[name]
  local function binary_helper(n,x,y)
    local i,s=0,.5
    while i<n do s=s+op(x+i*.125,y); i=i+1 end
    return s
  end
  run('math_'..name,binary_helper,24,.75,name=='ldexp' and 3 or 1.25)
  probe_flush()
  run('math_'..name..'_after_flush',binary_helper,24,.75,
      name=='ldexp' and -2 or -.75)
end
local atan2_builtin,pow_builtin,ldexp_builtin=math.atan2,math.pow,math.ldexp
local function binary_composition(n,x,y,p)
  local i,s=0,.5
  while i<n do
    local a=atan2_builtin(x+i*.125,y)
    local b=pow_builtin(a+2.5,y)
    local c=ldexp_builtin(a,p)
    s=s+b+c+a
    i=i+1
  end
  return s
end
run('math_binary_helpers_composed',binary_composition,12,.75,1.25,3)
local function binary_special(name,op,x,y)
  local function kernel(n,a,b)
    local i,r=0,.5
    while i<n do r=op(a,b); i=i+1 end
    return r
  end
  jit.off(kernel);local expected=kernel(8,x,y);jit.on(kernel)
  jit.flush();jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1','-fma')
  assert(same_number(kernel(8,x,y),expected),name..' warmup')
  probe_reset()
  assert(same_number(kernel(8,x,y),expected),name..' native')
  probe_check('math_'..name)
  collectgarbage('collect');probe_reset()
  assert(same_number(kernel(8,x,y),expected),name..' GC reentry')
  probe_check('math_'..name)
end
jit.off(binary_special,true)
binary_special('atan2_negative_zero',atan2_builtin,-0.0,1.25)
binary_special('atan2_negative_axis',atan2_builtin,-0.0,-1.25)
binary_special('atan2_nan',atan2_builtin,0/0,1.25)
binary_special('pow_negative_zero',pow_builtin,-0.0,3)
binary_special('pow_nan',pow_builtin,-1.25,.5)
binary_special('pow_infinity',pow_builtin,1/0,1.25)
binary_special('ldexp_negative_zero',ldexp_builtin,-0.0,3)
binary_special('ldexp_underflow',ldexp_builtin,.75,-1074)
binary_special('ldexp_overflow',ldexp_builtin,1.5,1024)
local log_builtin,sin_builtin,cos_builtin=math.log,math.sin,math.cos
local function helper_composition(n,x)
  local i,s,t=0,.25,.75
  while i<n do
    local a=sin_builtin(x+i*.01)
    local b=cos_builtin(a)
    s=s+log_builtin(b+2,3)
    t=t+a*b
    i=i+1
  end
  return s+t
end
run('math_helpers_composed',helper_composition,16,.25)
local function helper_snapshot_pair(n,x)
  local i,sum,ret=0,.25,x
  while i<n do
    local y=(ret+.5)/1.25
    sum=sum+sin_builtin(y)
    local b=cos_builtin(y)
    ret=y
    i=i+1
  end
  return sum+ret
end
run('math_helpers_snapshot_pair',helper_snapshot_pair,16,.25)
local function math_computed_result(n,x)
  local i,r=0,.5
  while i<n do
    i=i+1
    r=sin_builtin(x+i*.25)
  end
  return r
end
run_native('math_computed_result',math_computed_result,20,4.75)
collectgarbage('collect')
probe_reset()
assert(same_number(math_computed_result(20,4.75),
                   math.sin(4.75+20*.25)))
probe_check('math_computed_result')

assert(release:send(true,5))
assert(peer:join(5))
