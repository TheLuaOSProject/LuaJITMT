-- Late effects must retain the immediate userdata method lookup in the loop.
local ffi, util, vmdef, bit = require('ffi'), require('jit.util'), require('jit.vmdef'), require('bit')
local mode=assert(arg[1]);local p=assert(io.tmpfile());local close=p.close
local ct=ffi.typeof('struct {double x;}');local direct=ct(0)
local ptr=ffi.new('double[1]');local indirect=ffi.new('struct {double *ptr;}');indirect.ptr=ptr
ffi.cdef('int abs(int);');local fn=ffi.C.abs
local state={counter=0};local clear=require('table.clear')
local runs={
 luastore=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;state.counter=i end;return sum end,
 newref=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;state[i+1000]=i end;return sum end,
 allocate=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;local q=ct(i);sum=sum+q.x end;return sum end,
 indirect=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;indirect.ptr[0]=i end;return sum end,
 directstore=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;direct.x=i end;return sum end,
 clear=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;clear(state) end;return sum end,
 foreign=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;sum=sum+fn(-i) end;return sum end,
 fpmath=function(p,n,state,ct,indirect,fn,direct,clear) local sum=0;for i=1,n do if p.write then sum=sum+1 end;sum=sum+math.sin(i) end;return sum end,
}
local exits={}
local function onexit(tr) exits[tr]=(exits[tr] or 0)+1 end
local function main()
 local run=assert(runs[mode]);jit.flush();jit.opt.start('hotloop=1','hotexit=1000');jit.attach(onexit,'texit')
 local value=run(p,80,state,ct,indirect,fn,direct,clear)
 local root
 for tr=1,200 do local ti=util.traceinfo(tr);if ti and ti.link==tr and ti.linktype=='loop' then root=tr;break end end
 assert(root and (exits[root] or 0)>0,'actual native self-linked root')
 local ti=util.traceinfo(root);local post=false;local meta,postmeta,polls=0,0,0;local ops={}
 for ref=1,ti.nins do local _,ot,a,b=util.traceir(root,ref);if ot then
  local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6);ops[op]=(ops[op] or 0)+1
  if op=='LOOP  ' then post=true end
  if op=='XPOLL ' then assert(a==0);polls=polls+1 end
  if op=='FLOAD ' and vmdef.irfield[b]=='udata.meta' then if post then postmeta=postmeta+1 else meta=meta+1 end end
 end end
 assert(meta==1 and polls==1)
 if mode=='directstore' then assert(postmeta==0 and ops['XSTORE'] and direct.x==80)
 else assert(postmeta==1,'excluded body must retain copied userdata method proof') end
 if mode=='allocate' then assert(ops['CNEW  '] and value==3320)
 elseif mode=='luastore' then assert(ops['HSTORE'] and state.counter==80 and value==80)
 elseif mode=='newref' then assert(ops['NEWREF'] and state[1080]==80 and value==80)
 elseif mode=='indirect' then assert(ops['XSTORE'] and indirect.ptr[0]==80 and value==80)
 elseif mode=='clear' then assert(ops['CALLS '] and next(state)==nil and value==80)
 elseif mode=='foreign' then assert(ops['CALLXS'] and value==3320)
 elseif mode=='fpmath' then local expected=0;for i=1,80 do expected=expected+1+math.sin(i) end;assert(ops['CALLN '] and math.abs(value-expected)<1e-9)
 else assert(value==80) end
 print('udata-effect',mode,'root',root,'nins',ti.nins,'postmeta',postmeta,'value',value,'exits',exits[root])
end
jit.off(onexit,true);jit.off(main,true);local ok,err=pcall(main);jit.attach(onexit);close(p);assert(ok,err)
