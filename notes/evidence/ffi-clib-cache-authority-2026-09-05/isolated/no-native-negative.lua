local ffi, th, util = require('ffi'), require('threading'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local kind, mode, target, so, expect_helper = assert(arg[1]), assert(arg[2]), assert(arg[3]), assert(arg[4]), arg[5]=='helper'
ffi.cdef[[
extern int reg_clib_root_slot, reg_clib_side_slot, reg_clib_other_slot;
int reg_clib_root_fn(void); int reg_clib_side_fn(void); int reg_clib_other_fn(void);
enum { REG_CLIB_ROOT_ZERO=0, REG_CLIB_SIDE_ZERO=0,
       REG_CLIB_ROOT_BIG=4294967295U, REG_CLIB_SIDE_BIG=4294967295U };
]]
local gc_enabled=arg[6]=='gc'
local a,b,observer=ffi.load(so),ffi.load(so),ffi.load(so)
local index,newindex,close=debug.getmetatable(a).__index,debug.getmetatable(a).__newindex,debug.getmetatable(a).__gc
local ea,eb=debug.getfenv(a),debug.getfenv(b)
local rootkey,sidekey,otherkey
if kind=='function' then rootkey,sidekey,otherkey='reg_clib_root_fn','reg_clib_side_fn','reg_clib_other_fn'
elseif kind=='read' or kind=='write' then rootkey,sidekey,otherkey='reg_clib_root_slot','reg_clib_side_slot','reg_clib_other_slot'
elseif kind=='zero' then rootkey,sidekey='REG_CLIB_ROOT_ZERO','REG_CLIB_SIDE_ZERO'
elseif kind=='big' then rootkey,sidekey='REG_CLIB_ROOT_BIG','REG_CLIB_SIDE_BIG'
else error(kind) end
local ra,rb=index(a,rootkey),index(b,sidekey)
local replacement=otherkey and index(target=='root' and a or b,otherkey)
if kind=='write' then replacement=nil end
local env,key,receiver=target=='root' and ea or eb,target=='root' and rootkey or sidekey,target=='root' and a or b
local old=env[key]
local alternate=otherkey and env[otherkey]
if mode=='positive-zero' then env[key]=-0.0;if target=='root' then ra=-0.0 else rb=-0.0 end end
for _,key in ipairs({'reg_clib_root_slot','reg_clib_side_slot','reg_clib_other_slot'}) do index(observer,key) end
local effects={0};local out={};for i=1,80 do out[i]=false end
local function run_read(left,right,n,cut,rkey,skey,output)
 for i=0,n do if i>0 then
  effects[1]=effects[1]+1
  if i<cut then output[i]=index(left,rkey) else output[i]=index(right,skey) end
 end end
end
local function run_write(left,right,n,cut,rkey,skey,output)
 for i=0,n do if i>0 then
  effects[1]=effects[1]+1
  if i<cut then newindex(left,rkey,i) else newindex(right,skey,i+100) end
 end end
end
local run=kind=='write' and run_write or run_read
local roots,parents,exits,generations={},{},{},{}
local witness_generations
local function ontrace(what,tr,func,pc,parent)
 if what=='start' then generations[tr]=(generations[tr] or 0)+1 end
 if what=='start' and func==run then if parent and parent~=0 then parents[tr]=parent;roots[tr]=nil else roots[tr]=true;parents[tr]=nil end end
end
local function onexit(tr)
 if witness_generations and witness_generations[tr]~=generations[tr] then return end
 exits[tr]=(exits[tr] or 0)+1
end
local function reset_slots()
 newindex(observer,'reg_clib_root_slot',17);newindex(observer,'reg_clib_side_slot',41);newindex(observer,'reg_clib_other_slot',73)
end
local function check_output(left,right,limit)
 for i=1,limit or 80 do local value;if i<40 then value=left else value=right end
  assert(out[i]==value,'lookup result mismatch at '..i)
  if type(value)=='number' and value==0 then assert(1/out[i]==1/value,'zero sign changed at '..i) end
 end
end
local function trace_state()
 local rootexits,sideexits=0,0;local sides={};local helpers=0;local calls={}
 for id,name in pairs(vmdef.ircall) do if name=='lj_tab_gettv_rooted_hit_try' then calls[id]=true end end
 for tr in pairs(roots) do if util.traceinfo(tr) then rootexits=rootexits+(exits[tr] or 0) end end
 for tr,parent in pairs(parents) do local ti=util.traceinfo(tr)
  if ti and roots[parent] and ti.link==parent then sides[#sides+1]=tr;sideexits=sideexits+(exits[tr] or 0) end
 end
 for tr in pairs(roots) do local ti=util.traceinfo(tr);if ti then
  for ref=1,ti.nins do local _,ot,x,y=util.traceir(tr,ref);if ot then
   local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
   if op=='CALLS ' and calls[y] then helpers=helpers+1 end
  end end
 end end
 return rootexits,sideexits,sides,helpers
end
local stop,ready=th.channel(1),th.channel(1)
local worker=th.spawn(function(stop,ready) ready:send(true);local _,ok=stop:recv(20);assert(ok==true);return true end,stop,ready)
assert(select(2,ready:recv(10))==true)
local function main()
 if gc_enabled then collectgarbage('collect');collectgarbage('restart') else collectgarbage('stop') end
 jit.flush();jit.opt.start('hotloop=1','hotexit=1');jit.off(run,true);jit.attach(ontrace,'trace');jit.attach(onexit,'texit')
 for j=1,6 do effects[1]=0;reset_slots();run(a,b,80,40,rootkey,sidekey,out);assert(effects[1]==80)
  if kind~='write' then check_output(ra,rb) end
 end
 local warmroots,warmsides,oldsides,helpers=trace_state()
 if jit.status() then
  assert(warmroots>0 and warmsides>0 and #oldsides>0,'actual native root and installed side required')
  if expect_helper then assert(helpers>0,'shared root must contain real hit_try CALLS') end
 end
 local oldroots={}
 witness_generations={}
 for tr in pairs(roots) do if util.traceinfo(tr) then oldroots[tr]=true;witness_generations[tr]=generations[tr] end end
 for _,tr in ipairs(oldsides) do witness_generations[tr]=generations[tr] end
 if gc_enabled then collectgarbage('collect') end
 reset_slots()
 local expected_a,expected_b=ra,rb
 local must_error=(mode=='close' or mode=='between-close') or (kind=='write' and mode=='false')
 if mode=='false' then env[key]=false;if target=='root' then expected_a=false else expected_b=false end
 elseif mode=='nil' then env[key]=nil
 elseif mode=='other' then env[key]=alternate;if target=='root' then expected_a=replacement else expected_b=replacement end
 elseif mode=='negative-zero' then env[key]=-0.0;if target=='root' then expected_a=-0.0 else expected_b=-0.0 end
 elseif mode=='positive-zero' then env[key]=0.0;if target=='root' then expected_a=0.0 else expected_b=0.0 end
 elseif mode=='number' then env[key]=8589934591;if target=='root' then expected_a=8589934591 else expected_b=8589934591 end
 elseif mode=='fenv' then debug.setfenv(receiver,{[key]=false})
 elseif mode=='close' then close(receiver)
 elseif mode=='between-close' then assert(arm_clib_close)(receiver,key)
 else error(mode) end
 if gc_enabled then
  -- Only the namespace/trace roots retain its original env across collection.
  local weak=setmetatable({env,{}},{__mode='v'})
  ea,eb,env=nil,nil,nil
  collectgarbage('collect')
  assert(weak[2]==nil,'collection must clear an otherwise unreachable object')
  env=assert(weak[1],'namespace/trace must retain its original cache env')
  print('gc-lifetime','after-warm','after-mutation','original-env-alive')
 end
 effects[1]=0;exits={}
 local count=target=="root" and 39 or 80
 local ok,err=pcall(run,a,b,count,40,rootkey,sidekey,out)
 local rootexits=0;for tr in pairs(oldroots) do rootexits=rootexits+(exits[tr] or 0) end
 local sideexits=0;for _,tr in ipairs(oldsides) do sideexits=sideexits+(exits[tr] or 0) end
 local effect_expected=must_error and (target=='root' and 1 or 40) or count
 local av,bv,cv=index(observer,'reg_clib_root_slot'),index(observer,'reg_clib_side_slot'),index(observer,'reg_clib_other_slot')
 print('cache-authority',kind,mode,target,'warm',warmroots,warmsides,'helpers',helpers,'old-exits',rootexits,sideexits,'ok',ok,'effects',effects[1],'slots',av,bv,cv)
 assert(ok~=must_error,'lookup error behavior changed: '..tostring(err))
 assert(effects[1]==effect_expected,'lookup replayed or skipped a preceding effect')
 if mode=='close' or mode=='between-close' then assert(tostring(err):find('closed C library',1,true),'closed namespace error required') end
 if kind=='write' then
  local wanta,wantb,wantc=39,target=="root" and 41 or 180,73
  if must_error then wanta,wantb=target=='root' and 17 or 39,41
  elseif mode=='other' then if target=='root' then wanta,wantc=17,39 else wantb,wantc=41,180 end end
  assert(av==wanta and bv==wantb and cv==wantc,'extern store changed the wrong target')
 elseif not must_error then check_output(expected_a,expected_b,count) end
 if mode=='nil' then assert(env[key]~=nil,'native lookup skipped cache refill') end
 if jit.status() then assert(target=='root' and rootexits>0 or target=='side' and sideexits>0,'mutation must reach the original native target') end
end
for _,f in ipairs({ontrace,onexit,reset_slots,check_output,trace_state,main}) do jit.off(f,true) end
local ok,err=pcall(main)
jit.attach(ontrace);jit.attach(onexit);assert(stop:send(true,10)==true);local joined,result=worker:join(10);assert(joined==true and result==true)
assert(ok,err)
if mode=='between-close' then local hits,status,native=close_witness();assert(hits==1 and status==1 and native==1,'real successful native lookup must precede close');print('close-witness',hits,status,native) end
