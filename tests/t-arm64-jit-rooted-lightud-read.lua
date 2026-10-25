-- Exact segmented lightuserdata tokens through rooted reads.
local jit, util, bit = require('jit'), require('jit.util'), require('bit')
local vmdef = require('jit.vmdef')
local ffi, threading = require('ffi'), require('threading')
assert(jit.arch == 'arm64' and jit.status())
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5)); assert(select(2, rel:recv(120))); return true
end, ready, release)
assert(select(2, ready:recv(5)))
local keys = {lightud_zero, lightud_segmented}
assert(keys[1] ~= keys[2])

local function read_loop(t, key, step, n)
  local value, i = t[key], 0
  while i < n do value=t[key]; i=i+step end
  return value
end
local function read_loaded_key(t, holder, step, n)
  local key, i = holder.key, 0
  local value=t[key]
  while i < n do key=holder.key; value=t[key]; i=i+step end
  return value
end
-- Each iteration first obtains twelve LIGHTUD values from rooted VLOADs, then
-- uses all twelve as identity keys for more rooted reads. Results stay live
-- through the whole read chain and create real full-width spill pressure.
local function pressure(source, target, n)
  -- Keep the snapshot-only initial sum in the same NUM class as its updates.
  local i, sum = 0, 0.5
  while i < n do
    local k1=source.k1; local k2=source.k2; local k3=source.k3; local k4=source.k4
    local k5=source.k5; local k6=source.k6; local k7=source.k7; local k8=source.k8
    local k9=source.k9; local k10=source.k10; local k11=source.k11; local k12=source.k12
    local a1=target[k1]; local a2=target[k2]; local a3=target[k3]; local a4=target[k4]
    local a5=target[k5]; local a6=target[k6]; local a7=target[k7]; local a8=target[k8]
    local a9=target[k9]; local a10=target[k10]; local a11=target[k11]; local a12=target[k12]
    sum=a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12
    i=i+1
  end
  return sum
end
local function opname(op)
  local idx=bit.rshift(op,8); return vmdef.irnames:sub(idx*6+1,idx*6+6)
end
local function run(label, fn, args, t, key, expected, changed, checkid, newvalue)
  jit.flush(); jit.on(fn,true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=4','maxrecord=4000')
  local roots,aborts,exits={}, {}, 0
  local function trace(what,no,func,_,why,detail)
    if what=='start' and func==fn then roots[no]=true end
    if what=='abort' and roots[no] then roots[no]=nil; aborts[#aborts+1]=tostring(why)..':'..tostring(detail) end
  end
  local function exit(no) if roots[no] then exits=exits+1 end end
  jit.attach(trace,'trace'); jit.attach(exit,'texit')
  for _=1,20 do assert(fn(unpack(args))==expected,label..' initial') end
  local traces,reads,phis=0,0,0
  for no in pairs(roots) do
    local info=util.traceinfo(no)
    if info then
      traces=traces+1
      for ref=1,info.nins do
        local _,op,_,id=util.traceir(no,ref)
        if op then
          local name=opname(op)
          assert(name~='ALOAD ' and name~='HLOAD ' and name~='ASTORE' and name~='HSTORE' and name~='FSTORE',label..' raw heap IR')
          if name=='CALLS ' and vmdef.ircall[id]=='lj_tab_gettv_rooted_hit_try' then reads=reads+1 end
          if name=='PHI   ' and bit.band(op,31)==rooted_lightud_irtype then phis=phis+1 end
        end
      end
    end
  end
  assert(traces==1 and #aborts==0,label..' trace/abort: '..table.concat(aborts, ', '))
  assert(reads>=1,label..' rooted generic helper missing')
  assert(exits>=19 and exits<=21,label..' native exits '..exits)
  local warm_exits = exits
  if checkid then assert(phis > 0, label..' loop-carried lightuserdata PHI') end
  if checkid then assert(check_lightud_identity(fn(unpack(args)), expected), label..' original pointer') end
  if newvalue == nil then newvalue = changed end
  t[key]=newvalue
  local got=fn(unpack(args)); assert(got==changed,label..' changed')
  if checkid then assert(check_lightud_identity(got, changed), label..' changed pointer') end
  collectgarbage('collect')
  got=fn(unpack(args)); assert(got==changed,label..' after full GC')
  if checkid then assert(check_lightud_identity(got, changed), label..' post-GC pointer') end
  if fn == read_loop or fn == read_loaded_key then
    t[key] = nil
    assert(fn(unpack(args)) == nil, label..' missing key exit')
    t[key] = 39.5
    assert(fn(unpack(args)) == 39.5, label..' changed result type exit')
  end
  jit.attach(trace); jit.attach(exit)
  print(string.format('rooted lightuserdata read: %s calls=%d phis=%d warm_exits=%d total_exits=%d',label,reads,phis,warm_exits,exits))
end
jit.off(run,true)

local function closed_store(t, key, value, n)
  local i = 0
  while i < n do t[key] = value; i = i + 1 end
  return t[key]
end
local function check_closed_store(label, key, value)
  -- Give each rejection case its own bytecode and hotcount history.
  local fn = assert(loadstring(string.dump(closed_store)))
  jit.flush(); jit.on(fn, true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=4','maxrecord=4000')
  local t, roots, aborts = {[key] = value}, {}, 0
  local function trace(what, no, func)
    if what == 'start' and func == fn then roots[no] = true end
    if what == 'abort' and roots[no] then roots[no] = nil; aborts = aborts + 1 end
  end
  jit.attach(trace, 'trace')
  for _ = 1, 20 do assert(fn(t, key, value, 32) == value) end
  jit.attach(trace)
  for no in pairs(roots) do assert(not util.traceinfo(no), label..' admitted') end
  assert(aborts > 0, label..' did not exercise recording')
  print('rooted lightuserdata closed store: '..label)
end
jit.off(check_closed_store, true)

if rooted_lightud_read_admission then
  rooted_lightud_read_trace_clone=assert(loadstring(string.dump(pressure)))
  local source,target={},{}
  for i=1,12 do local k=_G['lightud_key'..i]; source['k'..i]=k; target[k]=i*10.25 end
  local expected=0; for i=1,12 do expected=expected+i*10.25 end
  run('VLOAD pressure',pressure,{source,target,32},target,lightud_key1,
      expected,expected+1,false,11.25)
elseif rooted_lightud_sload_admission then
  rooted_lightud_read_trace_clone=assert(loadstring(string.dump(read_loop)))
  local t,key={},lightud_segmented; t[key]=37.25
  run('SLOAD admission',read_loop,{t,key,1,32},t,key,37.25,41.5)
else
  local gc_values={{},'lightud-result',function() return 1 end,coroutine.create(function() end),newproxy(true),ffi.new('int[1]',7)}
  for _,key in ipairs(keys) do
    local other=key==keys[1] and keys[2] or keys[1]
    local cases={{'integer',17,29,false},{'number',17.25,29.75,false},
      {'true',true,true,false},{'false',false,false,false},
      {'lightuserdata',other,key,true}}
    for _,c in ipairs(cases) do
      local t={[key]=c[2]}
      run('key/'..c[1],read_loop,{t,key,1,32},t,key,c[2],c[3],c[4])
    end
    for _,value in ipairs(gc_values) do
      local t={[key]=value}
      run('key/GC result',read_loop,{t,key,1,32},t,key,value,value,false)
    end
  end
  local holder,t,key={key=lightud_segmented},{},lightud_segmented
  t[key]='prior-vload'; run('VLOAD key/string',read_loaded_key,{t,holder,1,32},t,key,'prior-vload','changed',false)
  local source,target={},{}
  for i=1,12 do local k=_G['lightud_key'..i]; source['k'..i]=k; target[k]=i*10.25 end
  local expected=0; for i=1,12 do expected=expected+i*10.25 end
  run('VLOAD pressure',pressure,{source,target,32},target,lightud_key1,
      expected,expected+1,false,11.25)
  check_closed_store('lightuserdata key', lightud_segmented, 17.25)
  check_closed_store('lightuserdata value', 'payload', lightud_segmented)
end
assert(release:send(true,5)); local joined,result=peer:join(5); assert(joined and result)
print('t-arm64-jit-rooted-lightud-read OK')
