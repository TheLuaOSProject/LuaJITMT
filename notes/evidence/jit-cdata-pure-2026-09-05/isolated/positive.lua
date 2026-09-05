local ffi = require('ffi')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local p = ffi.new('struct { double x; double y; }', 0, 1)
local function run(obj, n)
 local sum = 0
 for i=0,n do
  if i > 0 then
   obj.x = i
   sum = sum + obj.x + obj.y
  end
 end
 return sum
end
local exits = {}
local function onexit(tr) exits[tr] = (exits[tr] or 0)+1 end
local function check()
 jit.flush()
 jit.opt.start('hotloop=1','hotexit=1000')
 jit.attach(onexit,'texit')
 assert(run(p,80)==3320)
 local nr=0
 for tr=1,100 do
  local info=util.traceinfo(tr)
  if info then
   nr=nr+1
   print('TRACE',tr,info.link,info.linktype,info.nins,'exits',exits[tr] or 0)
   for ref=1,info.nins do
    local m,ot,a,b=util.traceir(tr,ref)
    if ot then
     local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
     print(ref,op,bit.band(ot,31),a,b)
    end
   end
  end
 end
 assert(nr>0)
end
jit.off(onexit,true)
jit.off(check,true)
check()
