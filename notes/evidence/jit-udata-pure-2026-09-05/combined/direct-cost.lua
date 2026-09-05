local ffi, util, vmdef, bit = require('ffi'), require('jit.util'), require('jit.vmdef'), require('bit')
local kind, n = assert(arg[1]), tonumber(arg[2]) or 20000000
local obj, close
ffi.cdef('int abs(int);')
if kind == 'clib' then obj = ffi.C; assert(obj.abs)
elseif kind == 'file' then obj = assert(io.tmpfile()); close = obj.close
elseif kind == 'buffer' then obj = require('string.buffer').new()
elseif kind == 'plain' then obj = newproxy(true); debug.getmetatable(obj).__index = {marker=true}
else error(kind) end
local lookup = {
 clib = function(p, count) local sum=0; for i=1,count do if p.abs then sum=sum+1 end end; return sum end,
 file = function(p, count) local sum=0; for i=1,count do if p.write then sum=sum+1 end end; return sum end,
 buffer = function(p, count) local sum=0; for i=1,count do if p.put then sum=sum+1 end end; return sum end,
 plain = function(p, count) local sum=0; for i=1,count do if p.marker then sum=sum+1 end end; return sum end,
}
local exits={}
local function onexit(tr) exits[tr]=(exits[tr] or 0)+1 end
local function main()
 local run=lookup[kind]
 jit.flush();jit.opt.start('hotloop=1','hotexit=1000');jit.attach(onexit,'texit')
 assert(run(obj,80)==80)
 local root
 for tr=1,200 do local ti=util.traceinfo(tr);if ti and ti.link==tr and ti.linktype=='loop' then root=tr;break end end
 assert(root and (exits[root] or 0)>0,'actual native root')
 local ti=util.traceinfo(root);local post=false;local meta,node,postmeta,postnode=0,0,0,0
 for ref=1,ti.nins do local _,ot,a,b=util.traceir(root,ref);if ot then
  local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
  if op=='LOOP  ' then post=true end
  if op=='XPOLL ' then assert(a==0) end
  if op=='FLOAD ' and vmdef.irfield[b]=='udata.meta' then if post then postmeta=postmeta+1 else meta=meta+1 end end
  if op=='FLOAD ' and vmdef.irfield[b]=='tab.node' then if post then postnode=postnode+1 else node=node+1 end end
 end end
 print('shape',kind,'root',root,'nins',ti.nins,'mcode',#util.tracemc(root),'meta',meta,'node',node,'postmeta',postmeta,'postnode',postnode,'exits',exits[root])
 jit.attach(onexit)
 local best=math.huge
 for pass=1,5 do local start=os.clock();assert(run(obj,n)==n);local sec=os.clock()-start;best=math.min(best,sec);print('sample',pass,string.format('%.9f',sec)) end
 print('result',kind,n,string.format('%.9f',best))
end
jit.off(onexit,true);jit.off(main,true);main()
if close then close(obj) end
