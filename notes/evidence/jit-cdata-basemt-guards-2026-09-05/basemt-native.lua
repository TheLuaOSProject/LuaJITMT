local ffi = require('ffi')
local util = require('jit.util')
local mode = assert(arg[1])
local n = 80
local ct = ffi.typeof('struct { int x; int y; }')
local oldmt = debug.getmetatable(ct)
local oldcall = oldmt.__call
local replacement = {}
for k,v in pairs(oldmt) do replacement[k]=v end
local calls = 0
replacement.__call = function(ctype, ...)
  calls = calls + 1
  return oldcall(ctype, ...)
end
local function run(count)
  local total = 0
  for i=1,count do
    local obj=ct()
    obj.x=i
    obj.y=i+1
    total=total+obj.x+obj.y
  end
  return total
end
local function traces()
  local found={}
  for i=1,200 do
    if util.traceinfo(i) then found[#found+1]=i end
  end
  return found
end
jit.off(traces, true)
local exits={}
local function onexit(tr)
  exits[tr]=(exits[tr] or 0)+1
end
jit.off(onexit,true)
local function exitcount(ids)
  local sum=0
  for _,tr in ipairs(ids) do sum=sum+(exits[tr] or 0) end
  return sum
end
jit.off(exitcount,true)
local armed=false
local callback_traces={}
local function ontrace(what)
  if what=='flush' and armed then
    armed=false
    assert(run(n)==n*(n+2))
    callback_traces=traces()
    io.stdout:write('flush_callback_traces=',#callback_traces,'\n')
  end
end
jit.off(ontrace,true)
jit.flush()
jit.opt.start('hotloop=1','hotexit=1000')
jit.attach(onexit,'texit')
assert(run(n)==n*(n+2))
local before=traces()
local before_exits=exitcount(before)
if jit.status() then
  assert(#before>0,'pre-mutation allocation loop must compile')
  assert(before_exits>0,'pre-mutation allocation loop must execute native code')
end
local ok,err=pcall(function()
  if mode=='replace' or mode=='reentrant' then
    if mode=='reentrant' then armed=true;jit.attach(ontrace,'trace') end
    debug.setmetatable(ct,replacement)
    jit.attach(ontrace)
  elseif mode=='inplace' then
    oldmt.__call=replacement.__call
  else error('unknown mode') end
  local after_mutation=traces()
  local pre_calls=calls
  exits={}
  local value=run(n)
  local after_run=traces()
  local after_exits=exitcount(after_run)
  io.stdout:write('mode=',mode,' before_traces=',#before,
    ' before_native_exits=',before_exits,' after_mutation_traces=',#after_mutation,
    ' after_run_traces=',#after_run,' after_native_exits=',after_exits,
    ' observed_calls=',calls-pre_calls,' expected_calls=',n,' value=',value,'\n')
  io.stdout:flush()
  assert(value==n*(n+2),'post-mutation field semantics')
  assert(calls-pre_calls==n,'post-mutation native code skipped current __call method')
  if mode=='replace' and jit.status() then
    assert(#after_mutation==0,'base replacement must flush old traces')
    assert(after_exits>0,'replacement method must execute through newly recorded native code')
  end
end)
armed=false
jit.attach(ontrace)
jit.attach(onexit)
oldmt.__call=oldcall
debug.setmetatable(ct,oldmt)
assert(ok,err)
print('basemt native mutation control passed')
