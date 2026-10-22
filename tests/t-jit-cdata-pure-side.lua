local ffi,util,vmdef,bit=require('ffi'),require('jit.util'),require('jit.vmdef'),require('bit')
local obj=ffi.new('struct { double x; double y; }',0,1)
local mt=debug.getmetatable(obj);local oldindex=mt.__index
local exits,parents,calls={},{},0
local function run(p,n,cut)
 local sum=0
 for i=0,n do
  if i>0 then
   if i<cut then p.x=i else p.x=i+1 end
   sum=sum+p.x+p.y
  end
 end
 return sum
end
local function onexit(tr) exits[tr]=(exits[tr] or 0)+1 end
local function ontrace(what,tr,func,pc,parent,exitno)
 if what=='start' and parent then parents[tr]=parent end
end
local function repl(p,k) calls=calls+1;return oldindex(p,k)+1000 end
local function report()
 for tr=1,100 do local t=util.traceinfo(tr);if t then print('trace',tr,'link',t.link,t.linktype,'parent',parents[tr] or 0,'exits',exits[tr] or 0) end end
end
local function main()
 jit.flush();jit.opt.start('hotloop=1','hotexit=1');jit.attach(onexit,'texit');jit.attach(ontrace,'trace')
 for j=1,6 do assert(run(obj,80,40)==3361) end
 report()
 local side
 for tr,parent in pairs(parents) do
  local t=util.traceinfo(tr)
  if parent~=0 and t and t.link==parent and (exits[tr] or 0)>0 then side=tr end
 end
 assert(side,'a side trace must link to a root and execute natively')
 local root=parents[side]
 local ti=assert(util.traceinfo(root));local loop,pre,post,poll=0,0,0,0
 for ref=1,ti.nins do local _,ot,a,b=util.traceir(root,ref);if ot then
  local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
  if op=='LOOP  ' then loop=ref end
  if op=='XPOLL ' then assert(a==0);poll=poll+1 end
  if op=='FLOAD ' and a==-1 and b==276 then if loop==0 then pre=pre+1 else post=post+1 end end
 end end
 assert(pre==1 and poll==1 and loop>0,'linked destination must retain entry proof and phase poll')
 assert(post==(arg[1]=='baseline' and 1 or 0),'linked root must exercise requested reuse shape')
 local before=exits[side] or 0
 mt.__index=repl;calls=0
 for j=1,4 do assert(run(obj,80,40)==3361+160000) end
 assert(calls==640,'linked execution must use the replaced method at every lookup')
 assert((exits[side] or 0)>before,'old side must execute after the mutation')
 report();print('side-link-pass','oldside',side,'before-exits',before,'calls',calls)
end
for _,f in ipairs({onexit,ontrace,report,main}) do jit.off(f,true) end
local ok,err=pcall(main)
mt.__index=oldindex;jit.attach(onexit);jit.attach(ontrace)
assert(ok,err)
