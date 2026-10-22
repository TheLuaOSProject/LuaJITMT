-- Isolated optimization proof: receiver allocation is outside the native loop.
local ffi, util, vmdef, bit = require('ffi'), require('jit.util'), require('jit.vmdef'), require('bit')
local ct = ffi.typeof('struct { double x; double y; }')
local obj = ct(0,1)
local mt = debug.getmetatable(obj)
local oldindex, oldnewindex = mt.__index, mt.__newindex
local n, calls, exits, events = 80, 0, {}, {}
local expect_hoisted = arg[2] ~= 'baseline'
local mode = arg[1] or 'index'
local function run(p, count)
 local sum=0
 for i=0,count do
  if i > 0 then
   p.x=i
   sum=sum+p.x+p.y
  end
 end
 return sum
end
local function onexit(tr) exits[tr]=(exits[tr] or 0)+1 end
local function ontrace(what,tr,func,pc,parent,exitno)
 if what=='stop' then events[tr]=true end
end
local function replindex(p,k) calls=calls+1;return oldindex(p,k)+1000 end
local function replnewindex(p,k,v) calls=calls+1;return oldnewindex(p,k,v+1000) end
local function make_method() return function(p,k) return k=='x' and 1 or 2 end end
local function count_exits()
 local total=0;for _,v in pairs(exits) do total=total+v end;return total
end
local function trace_shape(tr, expect)
 local info=assert(util.traceinfo(tr));local loop, roots,nodes,loads,guards,polls=0,0,0,0,0,0
 local postroots,postnodes,postloads=0,0,0
 for r=1,info.nins do
  local _,ot,a,b=util.traceir(tr,r)
  if ot then
   local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
   if op=='LOOP  ' then loop=r end
   if op=='XPOLL ' then assert(a==0,'eligible trace must retain mode-0 phase poll');polls=polls+1 end
   if op=='FLOAD ' and a==-1 and b==276 then
    if loop==0 then roots=roots+1 else postroots=postroots+1 end
   elseif op=='FLOAD ' and b==7 then
    if loop==0 then nodes=nodes+1 else postnodes=postnodes+1 end
   elseif op=='HLOAD ' then
    if loop==0 then loads=loads+1 else postloads=postloads+1 end
   elseif op=='EQ    ' and bit.band(ot,31)==8 and loop==0 then guards=guards+1 end
  end
 end
 assert(loop>0 and roots==1 and nodes==1 and loads==2 and guards==2 and polls==1,
        'root entry must retain exact base/node/two-method guards and phase poll')
 if expect then
  assert(postroots==0 and postnodes==0 and postloads==0,'eligible body must reuse only guarded pre-roll proof')
 else
  assert(postroots==1 and postnodes==1 and postloads==2,'baseline must retain copied lookup chain')
 end
 print('shape',tr,'nins',info.nins,'loop',loop,'post',postroots,postnodes,postloads)
end
local function main()
 jit.flush();jit.opt.start('hotloop=1','hotexit=1000')
 if mode=='methodlife' then mt.__index=make_method() end
 local weak=mode=='methodlife' and setmetatable({mt.__index},{__mode='v'})
 jit.attach(onexit,'texit');jit.attach(ontrace,'trace')
 assert(run(obj,n)==(mode=='methodlife' and 3*n or 3320))
 assert(count_exits()>0,'warm loop must execute native')
 local root
 for tr=1,200 do local info=util.traceinfo(tr);if info and info.link==tr and info.linktype=='loop' then root=tr;break end end
 assert(root,'warm loop root');trace_shape(root,expect_hoisted)
 local expected,expected_calls,err=3320+2000*n,2*n,false
 if mode=='index' then mt.__index=replindex
 elseif mode=='newindex' then mt.__newindex=replnewindex;expected,expected_calls=3320+1000*n,n
 elseif mode=='missing' then mt.__index=nil;err,expected_calls=true,0
 elseif mode=='nonfunction' then mt.__index=42;err,expected_calls=true,0
 elseif mode=='resize' then
  for i=1,256 do mt['pure_resize_'..i]=i end
  mt.__index=replindex;collectgarbage('collect')
 elseif mode=='methodlife' then
  mt.__index=replindex;collectgarbage('collect')
  assert(weak[1]~=nil,'guarded method must remain rooted by old native trace')
 elseif mode=='replace' then
  local nextmt={};for k,v in pairs(mt) do nextmt[k]=v end;nextmt.__index=replindex
  debug.setmetatable(obj,nextmt)
  assert(not util.traceinfo(root),'root replacement must flush the old trace')
 else error('unknown mode '..mode) end
 if mode~='replace' then assert(util.traceinfo(root),'in-place mutation must challenge old native guard') end
 calls,exits=0,{}
 local ok,value=pcall(run,obj,n)
 if err then assert(not ok,'absent/nonfunction method must be observed')
 else assert(ok,value);assert(value==expected,'current method result') end
 assert(calls==expected_calls,'every semantic lookup must use current method')
 assert(count_exits()>0,'mutation must be followed by real native execution')
 print('pure-method',mode,'exits',count_exits(),'calls',calls,'expected',expected_calls)
end
for _,f in ipairs({onexit,ontrace,count_exits,trace_shape,main}) do jit.off(f,true) end
local ok,err=pcall(main)
mt.__index,mt.__newindex=oldindex,oldnewindex
debug.setmetatable(obj,mt)
jit.attach(onexit);jit.attach(ontrace)
assert(ok,err)
