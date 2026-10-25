-- Unsunk table construction uses the existing published constructor, with
-- ordinary native-frame/GC handoff and full-width snapshot locations.
local jit, util, bit = require('jit'), require('jit.util'), require('bit')
local vmdef, threading = require('jit.vmdef'), require('threading')
assert(jit.arch=='arm64' and jit.status())
local request, response = threading.channel(2), threading.channel(2)
local peer = threading.spawn(function(req,res)
  assert(res:send('ready',5))
  while true do
    local msg,ok=req:recv(120); assert(ok)
    if msg=='done' then return true end
    assert(msg=='gc')
    for _=1,8 do collectgarbage('collect') end
    assert(res:send('collected',30))
  end
end,request,response)
assert(response:recv(5)=='ready')
local cases={
  {'empty',function(n)
    local t, i = {},0
    while i<n do t={}; i=i+1 end
    return t,i
  end},
  {'two live allocations',function(n)
    local a,b,i={},{},0
    while i<n do a={}; b={}; i=i+1 end
    return a,i,b
  end},
  {'FORL allocations',function(n)
    local t={}
    for i=1,n do t={} end
    return t,n
  end},
}
local function run(case)
  local name,fn=unpack(case)
  jit.flush()
  if tnew_set_capacity then tnew_set_capacity(fn) end
  jit.on(fn)
  jit.opt.start('hotloop=1','hotexit=200','maxtrace=8','-sink')
  collectgarbage('collect')
  local roots,exits,aborts={},0,0
  local reasons={}
  local function trace(what,no,func,_,reason,detail)
    if what=='start' and func==fn then roots[no]=true end
    if what=='abort' and roots[no] then
      roots[no]=nil; aborts=aborts+1
      reasons[#reasons+1]=tostring(reason)..':'..tostring(detail)
    end
  end
  local function exit(no) if roots[no] then exits=exits+1 end end
  local previous
  local function check(n)
    local t,count,other=fn(n)
    assert(type(t)=='table' and count==n,name)
    assert(next(t)==nil and t~=previous,name..': fresh empty identity')
    if other then assert(type(other)=='table' and t~=other and next(other)==nil,name) end
    previous=t
    return t,other
  end
  jit.attach(trace,'trace'); jit.attach(exit,'texit')
  for _=1,4 do check(32) end
  exits=0
  for _=1,20 do check(32) end
  jit.attach(trace); jit.attach(exit)
  local count,allocations=0,0
  for no in pairs(roots) do
    local info=util.traceinfo(no)
    if info then
      count=count+1
      assert(info.linktype=='loop' and info.link==no,name..': linkage')
      for ref=1,info.nins do
        local _,op=util.traceir(no,ref)
        if op then
          local idx=bit.rshift(op,8)
          local opname=vmdef.irnames:sub(idx*6+1,idx*6+6)
          if opname=='TNEW  ' then allocations=allocations+1 end
          assert(opname~='ASTORE' and opname~='HSTORE' and opname~='FSTORE',name)
        end
      end
    end
  end
  assert(count==1 and allocations>=2,name..': native allocation trace '..count..'/'..allocations..' '..table.concat(reasons,','))
  assert(exits>=20 and exits<=40,name..': native exits '..exits)
  collectgarbage('restart')
  local t,other=check(32)
  collectgarbage('collect')
  t.kept=37; assert(t.kept==37)
  if other then other.kept=53; assert(other.kept==53 and t.kept==37) end
  assert(request:send('gc',5))
  for _=1,160 do check(64) end
  assert(response:recv(30)=='collected')
  check(32)
  print(string.format('TNEW: %s traces=%d allocations=%d exits=%d warmup_aborts=%d',
    name,count,allocations,exits,aborts))
end
jit.off(run,true)
if tnew_admission then
  tnew_trace_clone=assert(loadstring(string.dump(cases[1][2])))
  run(cases[1])
else
  for _,case in ipairs(cases) do run(case) end
end
assert(request:send('done',5))
local ok,value=peer:join(30); assert(ok and value)
print('t-arm64-jit-tnew OK')
