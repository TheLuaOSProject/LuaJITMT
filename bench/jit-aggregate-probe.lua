-- Compare small aggregate CALLXS coverage with a live MT peer.
-- Build tests/t-ffi-callxs-sysv-small-aggregate-lib.c for the selected target.
-- Usage: LUA_PATH='src/?.lua;;' src/luajit bench/jit-aggregate-probe.lua LIB
-- A selected-root exit plus CALLXS IR is a native witness. Missing witnesses
-- do not prove that an architecture cannot compile the aggregate shape.
local ffi = require('ffi')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')
assert(jit.status(), 'JIT must be enabled')
jit.off(true, true)

ffi.cdef[[
typedef struct { uint32_t lo, hi; } AggregateIntPair;
typedef struct { float x, y; } AggregateFloatPair;
typedef struct { uint32_t value; } AggregateWord;
void lj_callxs_sysv_reset(void);
uint32_t lj_callxs_sysv_count(uint32_t);
AggregateIntPair lj_callxs_sysv_make_int(uint32_t);
uint32_t lj_callxs_sysv_take_int(AggregateIntPair, uint32_t);
AggregateFloatPair lj_callxs_sysv_make_sse(uint32_t);
double lj_callxs_sysv_take_sse(AggregateFloatPair, double);
AggregateWord lj_callxs_sysv_step_word(AggregateWord, uint32_t);
]]
local lib = ffi.load(assert(arg[1], 'aggregate fixture library is required'))
local cases = {
  {'int-result', 'local v=fn(i); sum=sum+tonumber(v.lo)+tonumber(v.hi)',
    lib.lj_callxs_sysv_make_int, false, 0},
  {'int-argument', 'sum=sum+fn(x,i)',
    lib.lj_callxs_sysv_take_int, ffi.new('AggregateIntPair',7,11), 1},
  {'sse-result', 'local v=fn(i); sum=sum+v.x+v.y',
    lib.lj_callxs_sysv_make_sse, false, 2},
  {'sse-argument', 'sum=sum+fn(x,.75)',
    lib.lj_callxs_sysv_take_sse, ffi.new('AggregateFloatPair',1.25,2.5), 3},
  {'word-both', 'local v=fn(x,i); sum=sum+v.value',
    lib.lj_callxs_sysv_step_word, ffi.new('AggregateWord',7), 9},
}

local function roots(fn)
  local found = {}
  for pc=0,util.funcinfo(fn).bytecodes-1 do
    local ins = util.funcbc(fn,pc)
    if ins then
      local op = bit.band(ins,255)
      local name = vmdef.bcnames:sub(op*6+1,op*6+6)
      if name == 'JFORL ' or name == 'JLOOP ' then
        found[bit.rshift(ins,16)] = true
      end
    end
  end
  return found
end

local function sample(case)
  local source = 'return function(n,fn,x) local sum=.5; for i=1,n do '..
    case[2]..' end; return sum end'
  local candidate = assert(loadstring(source,'@aggregate-probe'))()
  local oracle = assert(loadstring(source,'@aggregate-probe'))()
  jit.off(oracle,true)
  local exits,resumes = {},{}
  local function observe(no,exitno)
    -- Match the patched runner at delivery, before a trace ID can be reused.
    if roots(candidate)[no] then
      local key = no..':'..exitno
      exits[key] = (exits[key] or 0)+1
      local snap,pc = util.tracesnap(no,exitno,true)
      if snap and pc then
        local ins = util.funcbc(candidate,pc)
        local op = ins and bit.band(ins,255)
        local name = op and vmdef.bcnames:sub(op*6+1,op*6+6):gsub(' +$','')
        resumes[key] = 'pc'..pc..':'..tostring(name)..':ir'..snap[0]
      end
    end
  end
  jit.off(observe,true)
  jit.flush()
  jit.opt.start('hotloop=1','hotexit=1000','maxtrace=64')
  jit.attach(observe,'texit')
  jit.on()
  lib.lj_callxs_sysv_reset()
  local calls = 0
  local ok,err = xpcall(function()
    for iteration=1,24 do
      local n = iteration%2 == 0 and 48 or 64
      assert(candidate(n,case[3],case[4]) == oracle(n,case[3],case[4]),
        case[1]..' differs from interpreter')
      calls = calls+2*n
    end
    collectgarbage('collect')
    assert(candidate(32,case[3],case[4]) == oracle(32,case[3],case[4]),
      case[1]..' differs after GC')
    calls = calls+64
    assert(lib.lj_callxs_sysv_count(case[5]) == calls,
      case[1]..' omitted or replayed a foreign call')
  end,debug.traceback)
  jit.attach(observe)
  assert(ok,err)
  local rows,events = {},{}
  for no in pairs(roots(candidate)) do
    local info = util.traceinfo(no)
    if info then
      local callxs = 0
      for ref=1,info.nins do
        local _,ot = util.traceir(no,ref)
        if ot then
          local pos = 6*bit.rshift(ot,8)
          if vmdef.irnames:sub(pos+1,pos+6) == 'CALLXS' then
            callxs = callxs+1
          end
        end
      end
      rows[#rows+1] = no..':CALLXS='..callxs..':link='..info.linktype
    end
  end
  for key,count in pairs(exits) do
    events[#events+1] = key..'='..count..':'..(resumes[key] or 'resume-unavailable')
  end
  table.sort(rows)
  table.sort(events)
  io.write(string.format('%s\t%s\t25\t%d\t%s\t%s\n',case[1],jit.arch,
    calls,table.concat(rows,','),table.concat(events,',')))
end

local function main()
  local ready,stop = threading.channel(1),threading.channel(1)
  local peer = assert(threading.spawn(function(r,s)
    assert(r:send(true,5))
    assert(select(2,s:recv(120)))
    return true
  end,ready,stop))
  local ok,err = xpcall(function()
    assert(select(2,ready:recv(5)), 'MT peer did not start')
    io.write('case\tarch\toracle_checks\tforeign_calls\tlive_roots\troot_exits\n')
    for _,case in ipairs(cases) do sample(case) end
  end,debug.traceback)
  local stopped = stop:send(true,5)
  local joined,result = peer:join(5)
  assert(ok,err)
  assert(stopped and joined and result, 'MT peer did not stop and join')
end
jit.off(roots,true)
jit.off(sample,true)
jit.off(main,true)
main()
