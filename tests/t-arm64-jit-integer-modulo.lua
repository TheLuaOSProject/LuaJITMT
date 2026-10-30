-- Ordinary cross-architecture integer modulo and fallback checks.
local jit,util,bit,vmdef = require('jit'),require('jit.util'),require('bit'),require('jit.vmdef')
local threading=require('threading')
jit.off(true,true)
local ready,stop=threading.channel(1),threading.channel(1)
local peer=assert(threading.spawn(function(r,s)
  jit.off(true,true);assert(r:send(true,5));assert(select(2,s:recv(120)));return true
end,ready,stop))
assert(select(2,ready:recv(5)))
local function same(a,b)
  return type(a)==type(b) and (a==b or (type(a)=='number' and a~=a and b~=b)) and
    (a~=0 or 1/a==1/b)
end
local function make(kind,constant)
  local loop=kind=='for' and 'for i=0,n-1 do ' or 'local i=0; while i<n do '
  local advance=kind=='for' and '' or 'i=i+1; '
  local expr=constant and '(x+i)%7' or '(x+i)%d'
  local source='return function(n,x,d) local sum=.5; '..loop..
    'sum=sum+'..expr..'; '..advance..'end; return sum end'
  local chunk=assert(loadstring(source,'@integer-modulo-'..kind..'-'..tostring(constant)))
  jit.off(chunk,true);local f=chunk();jit.off(f,true);return f
end
for _,kind in ipairs({'for','while'}) do
 for _,constant in ipairs({true,false}) do
  jit.flush();jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')
  local f,oracle=make(kind,constant),make(kind,constant)
  jit.on(f,true)
  local function compare(n,x,d)
    local a,b=f(n,x,d);local c,e=oracle(n,x,d)
    assert(same(a,c) and same(b,e),'modulo mismatch')
  end
  for i=1,8 do compare(64,17,7) end
  local root
  for pc=0,util.funcinfo(f).bytecodes-1 do
    local ins=util.funcbc(f,pc)
    if ins then
      local off=bit.band(ins,255)*6;local op=vmdef.bcnames:sub(off+1,off+6)
      if op=='JFORL ' or op=='JLOOP ' then root=bit.rshift(ins,16) end
    end
  end
  assert(root,'missing modulo root '..kind..'-'..tostring(constant))
  local ti=assert(util.traceinfo(root));assert(ti.link==root and ti.linktype=='loop')
  local code,addr,offset=util.tracemc(root);assert(code and #code>0)
  local hasmod=false
  for ref=1,ti.nins do
    local _,op=util.traceir(root,ref)
    if op then local pos=bit.rshift(op,8)*6; if vmdef.irnames:sub(pos+1,pos+6)=='MOD   ' then hasmod=true end end
  end
  -- x64 single-number builds may choose the ordinary NUM arithmetic graph.
  if jit.arch=='arm64' then assert(hasmod,'selected ARM64 root lacks integer MOD') end
  local exits=0;local function observe(no)if no==root then exits=exits+1 end end
  jit.off(observe,true);jit.attach(observe,'texit')
  for phase=1,2 do
    local before=exits;if phase==2 then collectgarbage('collect') end
    for _,x in ipairs({0,17,-17,-2147483648,2147483583}) do
      for _,d in ipairs({1,-1,7,-7,2147483647,-2147483648}) do compare(64,x,d) end
    end
    assert(exits>before,'no native exits')
    local c,a,o=util.tracemc(root);assert(c==code and a==addr and o==offset)
  end
  jit.attach(observe)
  -- Arithmetic overflow, zero divisors and coercions must retain the stock
  -- interpreter result even when the recorded integer specialization exits.
  for _,row in ipairs({{64,2147483647,7},{4,-2147483648,-1},
    {8,0,0},{8,0,-0.0},{8,17,0},{8,17,2.5},{8,17.5,7},
    {8,1/0,7},{8,-1/0,7},{8,0/0,7},{8,17,1/0},{8,17,0/0},
    {8,'17','7'}}) do compare(unpack(row)) end
  for _,bad in ipairs({false,{},'not-a-number'}) do
    local ok=pcall(f,8,bad,7);local expected=pcall(oracle,8,bad,7);assert(ok==expected)
    if not constant then
      ok=pcall(f,8,17,bad);expected=pcall(oracle,8,17,bad);assert(ok==expected)
    end
  end
  print(jit.arch,kind,'constant='..tostring(constant),'root='..root,'exits='..exits,'integer_mod='..tostring(hasmod),'GC/code/edge oracles passed')
 end
end
assert(stop:send(true,5));assert(peer:join(5))
