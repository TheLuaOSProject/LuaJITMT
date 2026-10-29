local ffi = require('ffi')
local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')

-- Keep the harness and oracle interpreted; candidate prototypes are enabled
-- explicitly after construction.
jit.off(true, true)

if (jit.arch ~= 'x64' and jit.arch ~= 'arm64') or jit.os == 'Windows' then
  print('t-ffi-callxs-nested-hfa SKIP: Darwin ARM64 or SysV x64 only')
  return
end
if not os.getenv('LJ_ARM64_INT_AGGREGATE_SO') then
  print('t-ffi-callxs-nested-hfa SKIP: C helper library not configured')
  return
end

ffi.cdef[[
typedef struct { float lo, hi; } Arm64FloatPair;
typedef struct { Arm64FloatPair inner; } Arm64NestedFloatPair;
double arm64_take_nested_float_pair(Arm64NestedFloatPair,double);
double arm64_take_nested_float_pair_alt(Arm64NestedFloatPair,double);
void arm64_aggregate_reset(void);
uint32_t arm64_aggregate_count(uint32_t);
]]
local lib = ffi.load(assert(os.getenv('LJ_ARM64_INT_AGGREGATE_SO')))
local fn = lib.arm64_take_nested_float_pair
local one = ffi.new('Arm64NestedFloatPair', {{1.25, 2.5}})

local function source()
  return assert(loadstring([[
return function(n, fn, one)
  local sum = .5
  for i = 1, n do
    sum = sum + fn(one, 3.75)
  end
  return sum
end
]], '@nested-hfa-peer'))()
end

local candidate, oracle = source(), source()
jit.off(oracle, true)

local function loop_trace(fn)
  local fi = util.funcinfo(fn)
  if not fi or not fi.bytecodes then return nil end
  for pc = 1, fi.bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      if vmdef.bcnames:sub(op*6+1, op*6+6) == 'JFORL ' then
        local id = bit.rshift(ins, 16)
        local info = util.traceinfo(id)
        if info and info.linktype == 'loop' and info.link == id then
          return id
        end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local function trace_has_callxs(id)
  local info = util.traceinfo(id)
  if not info then return false end
  for ref = 1, info.nins do
    local _, typedop = util.traceir(id, ref)
    if typedop then
      local pos = 6 * bit.rshift(typedop, 8)
      if vmdef.irnames:sub(pos+1, pos+6) == 'CALLXS' then return true end
    end
  end
  return false
end
jit.off(trace_has_callxs, true)
local expected = 1.25+2*2.5+3.75
assert(fn(one, 3.75) == expected,
       'direct C ABI call has incorrect argument values')

local peer_ready, peer_release = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(ready, release)
  assert(ready:send(true, 5))
  assert(select(2, release:recv(120)))
  return true
end, peer_ready, peer_release))
assert(select(2, peer_ready:recv(5)), 'live MT peer did not become ready')

jit.flush(candidate)
jit.opt.start('hotloop=1', 'hotexit=1000')
jit.on(candidate, true)
lib.arm64_aggregate_reset()
for pass = 1, 8 do
  assert(candidate(128, fn, one) == oracle(128, fn, one),
         'native candidate differs from interpreted C oracle')
end
assert(lib.arm64_aggregate_count(62) == 2048,
       'peer C calls were lost or replayed')

local root_trace = loop_trace(candidate)
assert(root_trace, 'candidate JFORL did not publish a self-linked loop trace')
assert(trace_has_callxs(root_trace),
       'selected candidate root does not contain CALLXS')
local info = util.traceinfo(root_trace)
assert(info and info.linktype == 'loop',
       'selected nested HFA peer is not a completed loop trace')
local exits = 0
local function texit(no)
  if no == root_trace then exits = exits + 1 end
end
jit.off(texit, true)
jit.attach(texit, 'texit')
local before = lib.arm64_aggregate_count(62)
local code_before, address_before, loop_before = util.tracemc(root_trace)
assert(code_before and address_before and loop_before,
       'selected root has no readable native code')
collectgarbage('collect')
assert(candidate(32, fn, one) == oracle(32, fn, one),
       'native peer changed after full GC')
assert(lib.arm64_aggregate_count(62) == before + 64,
       'peer post-GC effects were lost or replayed')
assert(exits > 0, 'native peer root did not take an observed exit after GC')
assert(loop_trace(candidate) == root_trace,
       'candidate stopped using the exact selected root after full GC')
assert(trace_has_callxs(root_trace),
       'selected root lost its CALLXS identity after full GC')
local code_after, address_after, loop_after = util.tracemc(root_trace)
assert(code_after == code_before and address_after == address_before and
       loop_after == loop_before,
       'selected root native-code identity changed across full GC')
local after_gc_exits = exits
exits = 0
local alt = lib.arm64_take_nested_float_pair_alt
assert(candidate(32, alt, one) == oracle(32, alt, one),
       'alternate C callee differs from interpreted oracle')
assert(lib.arm64_aggregate_count(63) == 64,
       'alternate C callee effects were lost or replayed')
assert(exits > 0, 'alternate callee did not exit the selected root')
assert(loop_trace(candidate) == root_trace,
       'alternate callee displaced the selected root')
local final_code, final_address, final_loop = util.tracemc(root_trace)
assert(final_code == code_before and final_address == address_before and
       final_loop == loop_before, 'alternate callee changed selected code')
jit.attach(texit)
assert(peer_release:send(true, 5), 'peer release failed')
local peer_ok, peer_result = peer:join(5)
assert(peer_ok and peer_result, 'live MT peer did not join cleanly')
print('t-ffi-callxs-nested-hfa OK:', jit.arch, 'trace', root_trace,
      'post-GC-exits', after_gc_exits, 'alternate-exits', exits, 'peer', 'joined')
