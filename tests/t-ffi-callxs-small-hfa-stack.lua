local ffi = require('ffi')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')

if (jit.arch ~= 'x64' and jit.arch ~= 'arm64') or jit.os == 'Windows' then
  print('t-ffi-callxs-small-hfa-stack SKIP: Darwin ARM64 or SysV x64 only')
  return
end
if not os.getenv('LJ_ARM64_INT_AGGREGATE_SO') then
  print('t-ffi-callxs-small-hfa-stack SKIP: C helper library not configured')
  return
end

ffi.cdef[[
typedef struct { float value; } Arm64FloatOne;
double arm64_take_float_one_stack_prefix(double,double,double,double,
  double,double,double,double,float,Arm64FloatOne,float);
void arm64_aggregate_reset(void);
uint32_t arm64_aggregate_count(uint32_t);
]]
local lib = ffi.load(assert(os.getenv('LJ_ARM64_INT_AGGREGATE_SO')))
local fn = lib.arm64_take_float_one_stack_prefix
local one = ffi.new('Arm64FloatOne', 1.25)

local function source()
  return assert(loadstring([[
return function(n, fn, one)
  local sum = .5
  for i = 1, n do
    sum = sum + fn(1,2,3,4,5,6,7,8,1.5,one,2.5)
  end
  return sum
end
]], '@small-hfa-stack-peer'))()
end

local candidate, oracle = source(), source()
jit.off(oracle, true)
local expected = 1+4+9+16+25+36+49+64+13.5+12.5+27.5
assert(fn(1,2,3,4,5,6,7,8,1.5,one,2.5) == expected,
       'direct C ABI call has incorrect argument values')

jit.flush(candidate)
jit.opt.start('hotloop=1', 'hotexit=1000')
jit.on(candidate, true)
lib.arm64_aggregate_reset()
for pass = 1, 8 do
  assert(candidate(128, fn, one) == oracle(128, fn, one),
         'native candidate differs from interpreted C oracle')
end
assert(lib.arm64_aggregate_count(59) == 2048,
       'peer C calls were lost or replayed')

local native_call_trace
for tr = 1, 256 do
  local info = util.traceinfo(tr)
  if info then
    local has_call = false
    for ref = 1, info.nins do
      local _, typedop = util.traceir(tr, ref)
      if typedop then
        local pos = 6 * bit.rshift(typedop, 8)
        has_call = has_call or
          vmdef.irnames:sub(pos+1, pos+6) == 'CALLXS'
      end
    end
    if has_call then native_call_trace = tr; break end
  end
end
assert(native_call_trace, 'peer did not publish a CALLXS native trace')
local info = util.traceinfo(native_call_trace)
assert(info and info.linktype == 'loop',
       'selected one-float peer is not a completed loop trace')
local exits = 0
local function texit(no)
  if no == native_call_trace then exits = exits + 1 end
end
jit.off(texit, true)
jit.attach(texit, 'texit')
local before = lib.arm64_aggregate_count(59)
collectgarbage('collect')
assert(candidate(32, fn, one) == oracle(32, fn, one),
       'native peer changed after full GC')
assert(lib.arm64_aggregate_count(59) == before + 64,
       'peer post-GC effects were lost or replayed')
assert(exits > 0, 'native peer root did not take an observed exit after GC')
assert(util.traceinfo(native_call_trace),
       'native peer trace was lost across full GC')
jit.attach(texit)
print('t-ffi-callxs-small-hfa-stack OK:', jit.arch, 'trace', native_call_trace)
