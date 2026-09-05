local ffi,util,vmdef,bit,profile=require('ffi'),require('jit.util'),require('jit.vmdef'),require('bit'),require('jit.profile')
local p=ffi.new('struct {double x;double y;}',0,1)
local mt=debug.getmetatable(p);local oldindex=mt.__index
local calls,samples,exits=0,0,0
local function run(p,n)local s=0;for i=0,n do if i>0 then p.x=i;s=s+p.x+p.y end end;return s end
local function repl(p,k)calls=calls+1;return oldindex(p,k)+1000 end
local function onprofile()samples=samples+1;if samples==1 then mt.__index=repl end end
local function onexit()exits=exits+1 end
local function main()
 jit.flush();jit.opt.start('hotloop=1','hotexit=1');jit.attach(onexit,'texit')
 assert(run(p,80)==3320);assert(exits>0 and util.traceinfo(1))
 profile.start('i1',onprofile)
 assert(not util.traceinfo(1),'profile policy must flush old mode-0 trace')
 -- Compile once under profile policy before waiting for a real sample.
 local initial=calls;local ans=run(p,80);assert(ans==3320+1000*(calls-initial))
 local sawmode1,conservative=false,false
 for tr=1,100 do
  local ti=util.traceinfo(tr)
  if ti and ti.link==tr and ti.linktype=='loop' then
   local loop=0;local post=0;local mode1=false
   for r=1,ti.nins do local _,ot,a,b=util.traceir(tr,r);if ot then
    local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
    if op=='LOOP  ' then loop=r end
    if op=='XPOLL ' and a==1 then mode1=true end
    if loop>0 and op=='FLOAD ' and a==-1 and b==276 then post=post+1 end
   end end
   if mode1 then sawmode1=true;if post>0 then conservative=true end end
  end
 end
 assert(sawmode1 and conservative,'profile trace must keep mode-1 poll and repeated method proof')
 local deadline=os.clock()+1
 repeat
  initial=calls;ans=run(p,10000);assert(ans==50015000+1000*(calls-initial),'profile callback mutation continuation')
 until samples>0 or os.clock()>deadline
 profile.stop();assert(samples>0,'actual profile callback must execute');assert(calls>0,'callback replacement must be observed');assert(exits>0)
 print('profile-negative','mode1',sawmode1,'conservative',conservative,'samples',samples,'methodcalls',calls,'exits',exits)
end
for _,f in ipairs({onprofile,onexit,main})do jit.off(f,true)end
local ok,err=pcall(main);profile.stop();mt.__index=oldindex;jit.attach(onexit);assert(ok,err)
