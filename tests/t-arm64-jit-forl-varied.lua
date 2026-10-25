-- Each kernel must publish and enter its own FORL trace. The driver stays
-- interpreted, so its loop cannot satisfy the native-execution assertions.
local jit, util = require('jit'), require('jit.util')
assert(jit.arch == 'arm64' and jit.status())
local function sum(n, seed)
  local acc = seed
  for i=1,n do acc=acc+i end
  return acc
end
local function two(n, seed)
  local pad, up, down = 0, seed, 0
  for i=1,n do up=up+i; down=down-i; pad=pad+1 end
  return up, down, pad
end
local function signed(first, stop, step)
  local prefix, acc = 0, 0
  for i=first,stop,step do prefix=prefix+1; acc=acc+i end
  return acc, prefix
end
local function fraction(n, seed)
  local acc, other = seed, 1.5
  for i=1,n do acc=acc+i*0.5; other=other+0.25 end
  return acc, other
end
local cases = {
  {'one accumulator',sum,{40,3},{823}},
  {'three accumulators',two,{20,5},{215,-210,20}},
  {'positive dynamic step',signed,{2,20,3},{77,7}},
  {'negative dynamic step',signed,{20,2,-3},{77,7}},
  {'floating body',fraction,{40,0.5},{410.5,11.5}},
}
local function run(case)
  local name, fn, args, expected = unpack(case)
  jit.flush()
  jit.on(fn)
  jit.opt.start('hotloop=1','hotexit=200','maxtrace=8')
  local roots, exits, aborts = {}, 0, {}
  local function trace(what,no,func,_,reason,detail)
    if what=='start' and func==fn then roots[no]=true end
    if what=='abort' and roots[no] then
      roots[no]=nil
      aborts[#aborts+1]=tostring(reason)..':'..tostring(detail)
    end
  end
  local function exit(no) if roots[no] then exits=exits+1 end end
  local function check()
    local actual={fn(unpack(args))}
    assert(#actual==#expected,name..': result arity')
    for i=1,#expected do assert(actual[i]==expected[i],name..': result '..i) end
  end
  jit.attach(trace,'trace'); jit.attach(exit,'texit')
  for _=1,20 do check() end
  jit.attach(trace); jit.attach(exit)
  local count=0
  for no in pairs(roots) do
    local info=util.traceinfo(no)
    if info then
      assert(info.linktype=='loop' and info.link==no,name..': native loop linkage')
      local code=util.tracemc(no)
      assert(type(code)=='string' and #code>0,name..': native code')
      count=count+1
    end
  end
  assert(count==1 and #aborts==0,name..': admission '..table.concat(aborts,','))
  assert(exits>=19 and exits<=21,name..': native exits '..exits)
  collectgarbage('collect'); check()
  print(string.format('FORL: %s traces=%d exits=%d',name,count,exits))
end
jit.off(run,true)
for _,case in ipairs(cases) do run(case) end
print('t-arm64-jit-forl-varied OK')
