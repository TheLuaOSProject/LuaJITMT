local buffer, util, bit = require('string.buffer'), require('jit.util'), require('bit')
local names = require('jit.vmdef').bcnames
jit.opt.start('hotloop=20','hotexit=100')
local function check(name, create)
 jit.flush()
 local fn, prepare, verify = create()
 local pc, seen, root
 for i=1,util.funcinfo(fn).bytecodes-1 do
  local op=bit.band(util.funcbc(fn,i),255)*6
  if names:sub(op+1,op+6)=='FORL  ' then pc=i end
 end
 assert(pc)
 local function onexit(tr)
  local ins=util.funcbc(fn,pc)
  local op=bit.band(ins,255)*6
  if names:sub(op+1,op+6)=='JFORL ' and bit.rshift(ins,16)==tr then seen=true;root=tr end
 end
 jit.off(onexit)
 jit.attach(onexit,'texit')
 for _=1,8 do prepare();fn(1000);verify() end
 jit.attach(onexit)
 print(name, seen and 'native_loop_exit' or 'fallback_no_loop_exit',root or 0)
end
jit.off(check)
check('small_append',function()
 local b,p=buffer.new(),'x'
 return function(n)for _=1,n do b:put(p) end end,function()b:reset()end,function()assert(b:get()==p:rep(1000))end
end)
check('reset_put_get',function()
 local b,p=buffer.new(),('x'):rep(64)
 return function(n)for _=1,n do b:reset():put(p);assert(b:get()==p)end end,function()b:reset()end,function()assert(#b==0)end
end)
check('fifo_get',function()
 local b,p=buffer.new(),'x'
 return function(n)for _=1,n do assert(b:get(1)==p)end end,function()b:set(p:rep(1000))end,function()assert(#b==0)end
end)
check('set_get',function()
 local b,p=buffer.new(),('x'):rep(64)
 return function(n)for _=1,n do b:set(p);assert(b:get()==p)end end,function()b:reset()end,function()assert(#b==0)end
end)
check('stream_decode',function()
 local b,p=buffer.new(),buffer.encode(41)
 return function(n)for _=1,n do assert(b:decode()==41)end end,function()b:set(p:rep(1000))end,function()assert(#b==0)end
end)
