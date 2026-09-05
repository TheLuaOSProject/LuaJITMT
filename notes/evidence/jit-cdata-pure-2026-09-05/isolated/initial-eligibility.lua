local ffi,util,vmdef,bit=require('ffi'),require('jit.util'),require('jit.vmdef'),require('bit')
local mode=arg[1]
local ct=ffi.typeof('struct {double x;double y;}')
local p=ct(0,1)
local clear=require('table.clear')
ffi.cdef'int abs(int);'
local cb=ffi.cast('int(*)(int)',function(i)return i+1 end)
local ptr=ffi.new('double[1]')
local indirect=ffi.new('struct {double *ptr;}')
indirect.ptr=ptr
local state={counter=0}
local methods={
 allocate=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do local q=ct();q.x=i;p.x=i;sum=sum+p.x+p.y+q.x end;return sum
 end,
 luastore=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y;state.counter=i end;return sum
 end,
 newref=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y;state[i+1000]=i end;return sum
 end,
 clear=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y;clear(state) end;return sum
 end,
 foreign=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y+ffi.C.abs(-i) end;return sum
 end,
 callback=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y+cb(i) end;return sum
 end,
 indirect=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y;indirect.ptr[0]=i end;return sum
 end,
 fpmath=function(p,n,ct,state,indirect,cb)
  local sum=0;for i=1,n do p.x=i;sum=sum+p.x+p.y+math.sin(i) end;return sum
 end,
}
local exits={}
local function onexit(tr)exits[tr]=(exits[tr] or 0)+1 end
local function main()
 local run=assert(methods[mode]);jit.flush();jit.opt.start('hotloop=1','hotexit=1000');jit.attach(onexit,'texit')
 local answer=run(p,80,ct,state,indirect,cb)
 local root
 for tr=1,100 do local ti=util.traceinfo(tr);if ti and ti.link==tr and ti.linktype=='loop' then root=tr;break end end
 assert(root,'negative eligibility still needs an actual native root')
 assert((exits[root] or 0)>0,'negative eligibility needs actual native execution')
 local info=util.traceinfo(root);local loop=0;local after=0;local effects={}
 for r=1,info.nins do
  local _,ot,a,b=util.traceir(root,r)
  if ot then
   local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
   effects[op]=(effects[op] or 0)+1
   if op=='LOOP  ' then loop=r end
   if loop>0 and op=='FLOAD ' and a==-1 and b==276 then after=after+1 end
  end
 end
 print('negative',mode,'root',root,'nins',info.nins,'exits',exits[root],'postroot',after,'answer',answer)
 for op,num in pairs(effects)do print(op,num)end
 assert(after>0,'ineligible body must retain repeated cdata-base method proof')
 if mode=='allocate' then assert(effects['CNEW  '])
 elseif mode=='luastore' then assert(effects['HSTORE'] or effects['ASTORE']);assert(state.counter==80);assert(answer==3320)
 elseif mode=='newref' then assert(effects['NEWREF']);assert(state[1080]==80);assert(answer==3320)
 elseif mode=='clear' then assert(effects['CALLS ']);assert(next(state)==nil);assert(answer==3320)
 elseif mode=='foreign' or mode=='callback' then assert(effects['CALLXS']);assert(answer==(mode=='foreign' and 6560 or 6640))
 elseif mode=='indirect' then assert(effects['XSTORE']);assert(indirect.ptr[0]==80);assert(answer==3320)
 elseif mode=='fpmath' then assert(effects['FPMATH']) end
end
jit.off(onexit,true);jit.off(main,true)
main();jit.attach(onexit);cb:free()
