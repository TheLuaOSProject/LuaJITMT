local jit, util = require('jit'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local ffi = require('ffi')
assert(jit.status())

local function root_id(fn)
  for pc=0,util.funcinfo(fn).bytecodes-1 do
    local ins=util.funcbc(fn,pc)
    if ins then
      local op=bit.band(ins,0xff)
      local name=vmdef.bcnames:sub(op*6+1,op*6+6)
      if name=='JFORL ' or name=='JLOOP ' then
        return bit.rshift(ins,16), name
      end
    end
  end
end

local function has_rooted_get(no)
  local info=assert(util.traceinfo(no))
  local gets, raw=0,0
  for ref=1,info.nins do
    local _,op,_,id=util.traceir(no,ref)
    local name=op and vmdef.irnames:sub(bit.rshift(op,8)*6+1,
                                        bit.rshift(op,8)*6+6)
    if name=='CALLS ' and vmdef.ircall[id]=='lj_func_getuv_rooted_try' then
      gets=gets+1
    elseif name=='UREFC ' then
      raw=raw+1
    end
  end
  if jit.arch=='arm64' then assert(gets>0 and raw==0) end
  return gets,raw
end

local function make_reader(initial)
  local value=initial
  local function read(n)
    local out=value
    for i=1,n do out=value end
    return out
  end
  local function write(v) value=v end
  return read,write
end

local function run_read_case(name, initial, replacement)
  jit.flush()
  local read,write=make_reader(initial)
  jit.on(read,true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=16')
  for _=1,32 do assert(rawequal(read(24),initial),name..': warm value') end
  local no=assert(root_id(read),name..': missing no-peer loop root')
  local gets,raw=has_rooted_get(no)
  local exits=0
  local function onexit(id) if id==no then exits=exits+1 end end
  jit.off(onexit,true); jit.attach(onexit,'texit')
  collectgarbage('collect')
  for _=1,32 do assert(rawequal(read(24),initial),name..': post-GC value') end
  jit.attach(onexit)
  assert(exits>0,name..': selected root did not execute after GC')
  write(replacement)
  for _=1,8 do assert(rawequal(read(24),replacement),name..': changed cell') end
  print(string.format('no-peer UGET %-8s root=%d gets=%d raw=%d exits=%d',
                      name,no,gets,raw,exits))
end

run_read_case('NUM',3.5,5.25)
run_read_case('INT',37,-9)
run_read_case('STR','before','after')
run_read_case('FUNC',math.sin,math.cos)
run_read_case('NIL',nil,false)
run_read_case('BOOL',false,true)
run_read_case('TABLE',{x=1},{x=2})
run_read_case('TYPE',41,'changed-type')
local cdata1=ffi.new('int[1]',17)
local cdata2=ffi.new('int[1]',29)
run_read_case('CDATA',cdata1,cdata2)

local function math_candidate()
  local sin=math.sin
  return function(n,x)
    local sum=0
    for i=1,n do sum=sum+sin(x+i*0.125) end
    return sum
  end
end
local function byte_candidate()
  local byte=string.byte
  return function(s,n)
    local sum=0
    for i=1,n do sum=sum+byte(s,1) end
    return sum
  end
end
local function string_candidate()
  local sub=string.sub
  return function(s,n)
    local out=s
    for i=1,n do out=sub(s,-4,-5) end
    return out
  end
end

local measured={
  {name='math.sin',fn=math_candidate(),args={24,0.375},
   expected=(function() local v=0; for i=1,24 do v=v+math.sin(0.375+i*0.125) end; return v end)(),
   replacement=math.cos,
   replacement_expected=(function() local v=0; for i=1,24 do v=v+math.cos(0.375+i*0.125) end; return v end)()},
  {name='string.byte',fn=byte_candidate(),args={string.char(128,1,127,0,255),24},
   expected=128*24,
   replacement=function(s,i) return string.byte(s,i) end,
   replacement_expected=128*24},
  {name='string.sub',fn=string_candidate(),args={'a\0\255bcdef',24},expected='',
   replacement=function(s,a,b) return string.sub(s,a,b) end,
   replacement_expected=''},
}
for _,case in ipairs(measured) do
  jit.flush(); jit.on(case.fn,true)
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=16')
  for _=1,32 do assert(case.fn(unpack(case.args))==case.expected,case.name..': warm') end
  local no=assert(root_id(case.fn),case.name..': missing root')
  local gets,raw=has_rooted_get(no)
  local exits=0
  local function onexit(id) if id==no then exits=exits+1 end end
  jit.off(onexit,true); jit.attach(onexit,'texit')
  collectgarbage('collect')
  for _=1,32 do assert(case.fn(unpack(case.args))==case.expected,case.name..': GC') end
  jit.attach(onexit)
  assert(exits>0,case.name..': selected root did not execute after GC')
  local upvalue=debug.getupvalue(case.fn,1)
  assert(upvalue~=nil,case.name..': missing captured callee')
  debug.setupvalue(case.fn,1,case.replacement)
  for _=1,8 do
    assert(case.fn(unpack(case.args))==case.replacement_expected,
           case.name..': replacement callee')
  end
  debug.setupvalue(case.fn,1,case.name=='math.sin' and math.sin or
                   case.name=='string.byte' and string.byte or string.sub)
  for _=1,8 do assert(case.fn(unpack(case.args))==case.expected,
                      case.name..': restored callee') end
  print(string.format('no-peer UGET %-8s root=%d gets=%d raw=%d exits=%d',
                      case.name,no,gets,raw,exits))
end
