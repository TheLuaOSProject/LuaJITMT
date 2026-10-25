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

local function lightud_store(t, key, value, n)
  local i = 0
  while i < n do t[key] = value; i = i + 1 end
  return i
end
local function lightud_store_for(t, key, value, n)
  for i = 1, n do t[key] = value end
  return n
end

local function run_lightud_store(label, fn, key, value, expected_helper)
  jit.flush(); jit.on(fn, true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=4','maxrecord=4000')
  local t, roots, aborts, exits = {[key] = 'prior'}, {}, {}, 0
  local function trace(what, no, called, _, why, detail)
    if what == 'start' and called == fn then roots[no] = true end
    if what == 'abort' and roots[no] then
      roots[no] = nil; aborts[#aborts+1] = tostring(why)..':'..tostring(detail)
    end
  end
  local function onexit(no) if roots[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace'); jit.attach(onexit, 'texit')
  for _ = 1, 20 do assert(fn(t, key, value, 32) == 32) end
  local warm_exits = exits
  local native, stores = 0, 0
  for no in pairs(roots) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local name = opname(op)
          assert(name ~= 'ASTORE' and name ~= 'HSTORE' and name ~= 'FSTORE', label)
          if name == 'CALLS ' and vmdef.ircall[id] == expected_helper then
            stores = stores + 1
          end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
         label..': trace admission '..table.concat(aborts, ','))
  assert(warm_exits >= 19 and warm_exits <= 21,
         label..': native warm exits '..warm_exits)
  assert(stores >= 1, label..': missing '..expected_helper)
  assert(t[key] == value, label..': stored identity')
  collectgarbage('collect')
  assert(fn(t, key, value, 32) == 32 and t[key] == value,
         label..': post-GC identity')
  jit.attach(trace); jit.attach(onexit)
  print(string.format('rooted lightuserdata store: %s traces=%d stores=%d warm_exits=%d',
                      label, native, stores, warm_exits))
  return fn, t
end
jit.off(lightud_store, true)
jit.off(lightud_store_for, true)
jit.off(run_lightud_store, true)

local function check_absent_store_closed(key, value)
  jit.flush(); jit.on(lightud_store, true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=4','maxrecord=4000')
  local t, roots, aborts, exits = {[key] = 'present'}, {}, 0, 0
  local helper_snapshots, check_missing, missing_exits = {}, false, 0
  local function trace(what, no, fn)
    if what == 'start' and fn == lightud_store then roots[no] = true end
    if what == 'abort' and roots[no] then roots[no] = nil; aborts = aborts + 1 end
  end
  local function onexit(no, sn)
    if roots[no] then
      exits = exits + 1
      if check_missing and helper_snapshots[no][sn] then
        missing_exits = missing_exits + 1
      end
    end
  end
  jit.attach(trace, 'trace'); jit.attach(onexit, 'texit')
  for _ = 1, 20 do assert(lightud_store(t, key, value, 32) == 32) end
  local warm_exits = exits
  local native = 0
  for no in pairs(roots) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      helper_snapshots[no] = {}
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op and opname(op) == 'CALLS ' and
           vmdef.ircall[id] == 'lj_tab_storegc_rooted' then
          local _, guard = util.traceir(no, ref+1)
          assert(opname(guard) == 'NE    ', 'missing helper status guard')
          local selected
          for sn = 0, info.nexit-1 do
            if util.tracesnap(no, sn)[0] <= ref+1 then selected = sn end
          end
          helper_snapshots[no][assert(selected)] = true
        end
      end
    end
  end
  assert(native == 1 and aborts == 0, 'existing-key setup trace missing')
  assert(warm_exits >= 19 and warm_exits <= 21,
         'existing-key setup native exits '..warm_exits)
  t[key] = nil
  check_missing = true
  assert(lightud_store(t, key, value, 32) == 32 and t[key] == value,
         'native existing-key guard failed to fall back for a new key')
  assert(exits > warm_exits and missing_exits == 1,
         'missing-key helper path did not exit native trace before insertion')
  jit.attach(trace); jit.attach(onexit)
end
jit.off(check_absent_store_closed, true)

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
elseif rooted_lightud_store_admission then
  rooted_lightud_store_trace_clone=assert(loadstring(string.dump(lightud_store)))
  run_lightud_store('LOOP LIGHTUD key/value', lightud_store, lightud_segmented, lightud_zero, 'lj_tab_storegc_rooted')
elseif rooted_lightud_store_for_admission then
  rooted_lightud_store_for_trace_clone=assert(loadstring(string.dump(lightud_store_for)))
  run_lightud_store('FORL LIGHTUD key/value', lightud_store_for, lightud_segmented, lightud_zero, 'lj_tab_storegc_rooted')
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
  run_lightud_store('LOOP LIGHTUD key/number value', lightud_store, lightud_segmented, 17.25, 'lj_tab_storenum_rooted')
  run_lightud_store('LOOP LIGHTUD key/bool value', lightud_store, lightud_segmented, true, 'lj_tab_storebool_rooted')
  run_lightud_store('LOOP string key/LIGHTUD value', lightud_store, 'lightuserdata-value', lightud_segmented, 'lj_tab_storegc_rooted')
  run_lightud_store('LOOP LIGHTUD key/GC value', lightud_store, lightud_segmented, {}, 'lj_tab_storegc_rooted')
  run_lightud_store('LOOP GC key/LIGHTUD value', lightud_store, {}, lightud_zero, 'lj_tab_storegc_rooted')
  run_lightud_store('LOOP LIGHTUD key/value', lightud_store, lightud_segmented, lightud_zero, 'lj_tab_storegc_rooted')
  run_lightud_store('FORL LIGHTUD key/number value', lightud_store_for, lightud_segmented, 23.75, 'lj_tab_storenum_rooted')
  run_lightud_store('FORL GC key/LIGHTUD value', lightud_store_for, {}, lightud_segmented, 'lj_tab_storegc_rooted')
  check_absent_store_closed(lightud_segmented, lightud_zero)
end
assert(release:send(true,5)); local joined,result=peer:join(5); assert(joined and result)
print('t-arm64-jit-rooted-lightud-read OK')
