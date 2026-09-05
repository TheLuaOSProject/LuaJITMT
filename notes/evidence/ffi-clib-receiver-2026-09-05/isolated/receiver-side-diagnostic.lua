local ffi, util = require('ffi'), require('jit.util')
ffi.cdef('extern int review_clib_slot; int review_clib_symbol(void);')
local mode = assert(arg[1])
local a, b = ffi.load(assert(arg[2])), ffi.load(assert(arg[3]))
local index = debug.getmetatable(a).__index
local newindex = debug.getmetatable(a).__newindex
assert(a.review_clib_slot == 11 and b.review_clib_slot == 29)
local effects, exits, roots, parents = {0}, {}, {}, {}
local run
if mode:match('^newindex') then
  run = function(receiver, n, cut, other)
    local sum = 0
    for i=0,n do
      if i > 0 then
        effects[1] = effects[1] + 1
        if i < cut then newindex(receiver, 'review_clib_slot', i)
        else newindex(other, 'review_clib_slot', i + 100) end
        sum = sum + i
      end
    end
    return sum
  end
else
  run = function(receiver, n, cut, other)
    local sum = 0
    for i=0,n do
      if i > 0 then
        effects[1] = effects[1] + 1
        if i < cut then sum = sum + index(receiver, 'review_clib_slot')
        else sum = sum + index(other, 'review_clib_slot') + 100 end
      end
    end
    return sum
  end
end
local function onexit(tr,ex) print('EXIT',tr,ex);exits[tr]=(exits[tr] or 0)+1 end
local function ontrace(what,tr,func,pc,parent)
  if what=='start' then print('tracestart',tr,func==run,pc,parent) end
  if what=='start' and func==run then
    if parent and parent~=0 then parents[tr]=parent else roots[tr]=true end
  end
end
local function rootexits()
  local n=0
  for tr in pairs(roots) do if util.traceinfo(tr) then n=n+(exits[tr] or 0) end end
  return n
end
local function configure()
  jit.flush()
  jit.opt.start('hotloop=1','hotexit=1')
  jit.attach(onexit,'texit');jit.attach(ontrace,'trace')
end
local function warm()
  local result
  for j=1,6 do
    a.review_clib_slot=11
    result=run(a,80,40,a)
    assert(result==(mode:match('^newindex') and 3240 or 4980))
  end
  local warmexits=rootexits()
  local side
  for tr,parent in pairs(parents) do
    local ti=util.traceinfo(tr)
    if ti and roots[parent] and ti.link==parent and (exits[tr] or 0)>0 then side=tr end
  end
  if jit.status() then
    assert(warmexits>0,'receiver loop must execute a native root')
    assert(side,'receiver loop must execute a side trace linked back to the root')
  end
  return warmexits,side
end
local function lifetime_warm()
  configure()
  local warmexits,side=warm()
  local weak=setmetatable({a},{__mode='v'})
  a=nil
  return weak,warmexits,side
end
local function main()
  if mode:match('life$') then
    local weak,warmexits,side=lifetime_warm()
    collectgarbage('collect')
    print(mode,'warmexits',warmexits,'side',side,'retained',weak[1]~=nil)
    if jit.status() then assert(weak[1]~=nil,'trace must retain its specialized namespace') end
    return
  end
  configure()
  local warmexits,side=warm()
  a.review_clib_slot=-101;b.review_clib_slot=29
  effects[1]=0;exits={}
  local sidechange=mode:match('side')~=nil
  local other=mode:match('type$') and io.stdout or b
  local receiver=sidechange and a or other
  local ok,result=pcall(run,receiver,80,40,other)
  local nativeexits=rootexits()
  for tr=1,30 do local ti=util.traceinfo(tr);if ti then print('traceinfo',tr,'root',roots[tr],'parent',parents[tr],'link',ti.link,ti.linktype,'exit',exits[tr]) end end
  local av,bv=a.review_clib_slot,b.review_clib_slot
  print(mode,'warmexits',warmexits,'side',side,'ok',ok,'result',result,'effects',effects[1],'a',av,'b',bv,'nativeexits',nativeexits,'sideexits',side and (exits[side] or 0) or 0)
  if mode:match('type$') then
    assert(not ok,'captured namespace method accepted a different userdata subtype')
    assert(effects[1]==(sidechange and 40 or 1),'throwing namespace lookup must not replay the preceding store')
    assert(av==(sidechange and mode:match('^newindex') and 39 or -101) and bv==29,'rejected receiver must not write the wrong namespace')
  else
    assert(ok,result)
    assert(effects[1]==80,'side exits must not replay the preceding store')
    if mode:match('^newindex') then
      assert(result==3240 and av==(sidechange and 39 or -101) and bv==180,'direct store used the wrong namespace')
    else
      assert(result==(sidechange and 1350 or 6420) and av==-101 and bv==29,'direct load used the wrong namespace')
    end
  end
  if jit.status() then
    if sidechange then assert((exits[side] or 0)>0,'changed receiver must reach the installed side trace')
    else assert(nativeexits>0,'changed receiver must reach an old native root') end
  end
end
for _,f in ipairs({onexit,ontrace,rootexits,configure,warm,lifetime_warm,main}) do jit.off(f,true) end
local ok,err=pcall(main)
jit.attach(onexit);jit.attach(ontrace)
assert(ok,err)
