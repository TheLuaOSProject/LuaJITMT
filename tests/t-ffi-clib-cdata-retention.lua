local ffi, th, util = require('ffi'), require('threading'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local kind,target,so,enabled=assert(arg[1]),assert(arg[3]),assert(arg[4]),arg[6]~='off'
ffi.cdef[[
extern int reg_clib_root_slot,reg_clib_side_slot,reg_clib_other_slot;
int reg_clib_root_fn(void); int reg_clib_side_fn(void); int reg_clib_other_fn(void);
]]
if not enabled then jit.off() end
local a,b,observer=ffi.load(so),ffi.load(so),ffi.load(so)
local mt=debug.getmetatable(a)
local index,newindex,close=mt.__index,mt.__newindex,mt.__gc
local rk,sk,ak
if kind=='function' then rk,sk,ak='reg_clib_root_fn','reg_clib_side_fn','reg_clib_other_fn'
else rk,sk,ak='reg_clib_root_slot','reg_clib_side_slot','reg_clib_other_slot' end
index(a,rk);index(b,sk)
local receiver=target=='root' and a or b
local env,key=debug.getfenv(receiver),target=='root' and rk or sk
index(receiver,ak)
local alternate=env[ak]
local weak=setmetatable({env[key],{}},{__mode='v'})
local effects={0}
local function readloop(left,right,n,cut,rkey,skey)
 local count=0
 for i=0,n do if i>0 then
  effects[1]=effects[1]+1
  if i<cut then if index(left,rkey) then count=count+1 end
  else if index(right,skey) then count=count+1 end end
 end end
 return count
end
local function writeloop(left,right,n,cut,rkey,skey)
 for i=0,n do if i>0 then
  effects[1]=effects[1]+1
  if i<cut then newindex(left,rkey,i) else newindex(right,skey,i+100) end
 end end
end
local run=kind=='write' and writeloop or readloop
local roots,parents,generations,exits={},{},{},{}
local witness_generations
local function ontrace(what,tr,func,pc,parent)
 if what=='start' then generations[tr]=(generations[tr] or 0)+1 end
 if what=='start' and func==run then
  if parent and parent~=0 then parents[tr]=parent;roots[tr]=nil
  else roots[tr]=true;parents[tr]=nil end
 end
end
local function onexit(tr)
 if witness_generations and witness_generations[tr]~=generations[tr] then return end
 exits[tr]=(exits[tr] or 0)+1
end
local function trace_state()
 local re,se,helpers=0,0,0;local sides={};local calls={}
 for id,name in pairs(vmdef.ircall) do if name=='lj_tab_cmpcdata_kgc_rooted_try' then calls[id]=true end end
 for tr in pairs(roots) do local ti=util.traceinfo(tr);if ti then
  re=re+(exits[tr] or 0)
  for ref=1,ti.nins do local _,ot,x,y=util.traceir(tr,ref);if ot then
   if vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)=='CALLS ' and calls[y] then helpers=helpers+1 end
  end end
 end end
 for tr,parent in pairs(parents) do local ti=util.traceinfo(tr)
  if ti and roots[parent] and ti.link==parent then sides[#sides+1]=tr;se=se+(exits[tr] or 0) end
 end
 return re,se,sides,helpers
end
local function reset_slots()
 newindex(observer,'reg_clib_root_slot',17)
 newindex(observer,'reg_clib_side_slot',41)
 newindex(observer,'reg_clib_other_slot',73)
end
local stop,ready=th.channel(1),th.channel(1)
local worker=th.spawn(function(stop,ready)
 jit.off(true,true);ready:send(true)
 local _,ok=stop:recv(20);assert(ok==true);return true
end,stop,ready)
assert(select(2,ready:recv(10))==true)
local function main()
 collectgarbage('collect');collectgarbage('restart')
 jit.flush();jit.opt.start('hotloop=1','hotexit=1');jit.attach(ontrace,'trace');jit.attach(onexit,'texit')
 for j=1,6 do effects[1]=0;reset_slots();run(a,b,80,40,rk,sk);assert(effects[1]==80) end
 local re,se,sides,helpers=trace_state()
 if enabled then assert(re>0 and se>0 and #sides>0 and helpers>0,'actual shared native root and installed side required') end
 local oldroots={};witness_generations={}
 for tr in pairs(roots) do if util.traceinfo(tr) then oldroots[tr]=true;witness_generations[tr]=generations[tr] end end
 for _,tr in ipairs(sides) do witness_generations[tr]=generations[tr] end
 close(receiver);env[key]=nil
 local drained,cycles=false,0
 for i=1,8 do
  collectgarbage('collect');advance_retired();cycles=i
  if closed_cache_empty(receiver) then drained=true;break end
 end
 assert(drained,'real grace protocol must empty live and retired namespace caches')
 collectgarbage('collect');collectgarbage('collect')
 assert(weak[2]==nil,'public full collection must clear the unreachable table')
 print('retention-roots',kind,target,enabled and 'on' or 'off','cycles',cycles,'retired-empty',drained,'weak-cdata',weak[1]~=nil)
 if not enabled then
  assert(weak[1]==nil,'without native KGC the removed closed-cache cdata must be collectible')
  return
 end
 assert(weak[1]~=nil,'the old trace must retain its exact cdata KGC through full collection')
 env[key]=weak[1]
 arm_cmp_probe(receiver,key,alternate,env,weak[1])
 reset_slots();effects[1]=0;exits={}
 local ok,err=pcall(run,a,b,target=='root' and 39 or 80,40,rk,sk)
 assert(not ok and tostring(err):find('closed C library',1,true),'later semantic close guard must still reject')
 assert(effects[1]==(target=='root' and 1 or 40),'comparison or close exit replayed/skipped the prefix')
 local rex,sex=0,0
 for tr in pairs(oldroots) do rex=rex+(exits[tr] or 0) end
 for _,tr in ipairs(sides) do sex=sex+(exits[tr] or 0) end
 assert(target=='root' and rex>0 or target=='side' and sex>0,'reentry must execute the old native target')
 local av,bv,cv=index(observer,'reg_clib_root_slot'),index(observer,'reg_clib_side_slot'),index(observer,'reg_clib_other_slot')
 local wanta=kind=='write' and target=='side' and 39 or 17
 assert(av==wanta and bv==41 and cv==73,'closed lookup must not perform an extern store')
 local h,s,n,r,c,v,q,z=cmp_probe_witness()
 assert(h==1 and s==1 and n==1 and r==1 and c==1 and z==5,'real helper must compare retained KGC before close rejects')
 print('retention-native',kind,target,'helpers',helpers,'old-exits',rex,sex,'effects',effects[1],'status',s,'clean',r,c,'slots',av,bv,cv)
end
for _,f in ipairs({main,ontrace,onexit,trace_state,reset_slots}) do jit.off(f,true) end
local ok,err=pcall(main)
jit.attach(ontrace);jit.attach(onexit);assert(stop:send(true,10)==true)
local joined,result=worker:join(10);assert(joined==true and result==true)
assert(ok,err)
