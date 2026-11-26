local buffer, th, ffi = require('string.buffer'), require('threading'), require('ffi')
local util, bit = require('jit.util'), require('bit')
ffi.cdef[[struct timespec { long tv_sec; long tv_nsec; }; int clock_gettime(int, struct timespec *);]]
local stamp=ffi.new('struct timespec[1]')
local function wall()
 assert(ffi.C.clock_gettime(1,stamp)==0)
 return tonumber(stamp[0].tv_sec)+tonumber(stamp[0].tv_nsec)*1e-9
end
local policy,sample,count,rounds,period = arg[1],tonumber(arg[2]),tonumber(arg[3]),tonumber(arg[4]),tonumber(arg[5])
assert(jit.status());jit.opt.start('hotloop=20','hotexit=100')
local keys={}
for k,v in pairs(th.gcstats()) do if type(v)=='number' then keys[#keys+1]=k end end
table.sort(keys)
local columns={'policy','sample','stage','round','total_ops','work_cpu','work_wall','gc_cpu','gc_wall','lua_bytes'}
for _,k in ipairs(keys) do columns[#columns+1]='gc_'..k end
io.write(table.concat(columns,','),'\n');io.flush()
local function emit(stage,round,workcpu,workwall,gccpu,gcwall)
 bin_census(stage,round)
 local luabytes=collectgarbage('count')*1024
 local stats=th.gcstats()
 local row={policy,tostring(sample),stage,tostring(round),tostring(round*count),string.format('%.9f',workcpu),string.format('%.9f',workwall),string.format('%.9f',gccpu),string.format('%.9f',gcwall),string.format('%.0f',luabytes)}
 for _,k in ipairs(keys) do row[#row+1]=string.format('%.17g',stats[k]) end
 io.write(table.concat(row,','),'\n');io.flush()
 assert(io.read('*l')=='go','missing controller acknowledgement')
end
jit.off(emit)
local function make()
 local b,payload=buffer.new(),('x'):rep(1024)
 local function run(n)
  for _=1,n do b:reset():put(payload);assert(b:get()==payload) end
 end
 return b,run
end
local function suite()
 local b,run=make()
 run(1000)
 collectgarbage('collect')
 emit('warm',0,0,0,0,0)
 for round=1,rounds do
  local c0,w0=os.clock(),wall()
  run(count)
  local wc,ww=os.clock()-c0,wall()-w0
  assert(#b==0)
  emit('work',round,wc,ww,0,0)
  local gc,gw=0,0
  if period>0 and round%period==0 then
   c0,w0=os.clock(),wall();collectgarbage('collect');gc,gw=os.clock()-c0,wall()-w0
  end
  emit('after_gc',round,wc,ww,gc,gw)
 end
 b,run=nil,nil
 jit.flush()
 emit('dropped',rounds,0,0,0,0)
 for pass=1,5 do
  local c0,w0=os.clock(),wall();collectgarbage('collect')
  emit('quiescent_gc_'..pass,rounds,0,0,os.clock()-c0,wall()-w0)
 end
end
jit.off(suite)
suite()
