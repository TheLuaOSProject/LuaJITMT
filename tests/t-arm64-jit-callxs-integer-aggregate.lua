local ffi = require('ffi')
local jitutil = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')

jit.off(true, true)
ffi.cdef[[
typedef struct { uint32_t lo, hi; } Arm64IntPair;
typedef struct { float lo, hi; } Arm64FloatPair;
typedef struct { Arm64IntPair pair; uint32_t tag; } Arm64NestedPair;
typedef union { uint64_t u; double d; } Arm64MixedUnion;
typedef struct { uint8_t value; } Arm64Byte;
typedef struct { uint16_t value; } Arm64Half;
typedef struct { uint32_t value; } Arm64Word;
typedef struct { uint64_t lo, hi; } Arm64BigPair;
typedef struct { uint32_t value:5; } Arm64Bitfield;
void arm64_aggregate_reset(void);
uint32_t arm64_aggregate_count(uint32_t);
uint32_t arm64_take_int_pair(Arm64IntPair, uint32_t);
double arm64_take_int_pair_double_salt(Arm64IntPair, uint32_t);
uint32_t arm64_take_float_pair(Arm64FloatPair);
uint32_t arm64_take_nested_pair(Arm64NestedPair);
uint32_t arm64_take_union(Arm64MixedUnion);
uint32_t arm64_take_byte(Arm64Byte, uint32_t);
uint32_t arm64_take_half(Arm64Half, uint32_t);
uint32_t arm64_take_word(Arm64Word, uint32_t);
uint32_t arm64_take_big_pair(Arm64BigPair);
uint32_t arm64_take_bitfield(Arm64Bitfield);
uint32_t arm64_take_pair_no_gpr(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t,
                                Arm64IntPair);
]]
local lib = ffi.load(assert(os.getenv('LJ_ARM64_INT_AGGREGATE_SO')))
_G.__arm64_int_pair_fn = lib.arm64_take_int_pair
_G.__arm64_float_pair_fn = lib.arm64_take_float_pair
_G.__arm64_nested_pair_fn = lib.arm64_take_nested_pair
_G.__arm64_mixed_union_fn = lib.arm64_take_union
_G.__arm64_byte_fn = lib.arm64_take_byte
_G.__arm64_half_fn = lib.arm64_take_half
_G.__arm64_word_fn = lib.arm64_take_word
_G.__arm64_big_pair_fn = lib.arm64_take_big_pair
_G.__arm64_bitfield_fn = lib.arm64_take_bitfield
_G.__arm64_pair_no_gpr_fn = lib.arm64_take_pair_no_gpr

local candidate = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-candidate'))()
local oracle = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-oracle'))()
local clone = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-clone'))()
jit.off(oracle, true)
jit.off(clone, true)
_G.__arm64_integer_aggregate_candidate = candidate
_G.__arm64_integer_aggregate_clone = clone
_G.__arm64_integer_aggregate_oracle = oracle
_set_aggregate_candidate(candidate)

local pair = ffi.new('Arm64IntPair', 7, 11)
local byte = ffi.new('Arm64Byte', 7)
local half = ffi.new('Arm64Half', 701)
local word = ffi.new('Arm64Word', 70001)
local fn = lib.arm64_take_int_pair
local alt = lib.arm64_take_int_pair_double_salt
local function roots()
  local found = {}
  for pc = 0, jitutil.funcinfo(candidate).bytecodes-1 do
    local ins = jitutil.funcbc(candidate, pc)
    if ins then
      local op = bit.band(ins, 255)
      local name = vmdef.bcnames:sub(op*6+1, op*6+6)
      if name == 'JFORL ' or name == 'JLOOP ' then
        found[bit.rshift(ins, 16)] = true
      end
    end
  end
  return found
end
jit.off(roots, true)
local selected, exits = {}, 0
local function observe(no)
  if roots()[no] then exits = exits + 1 end
end
jit.off(observe, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(120)))
  return true
end, ready, stop))
assert(select(2, ready:recv(5)), 'live peer did not start')

jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')
jit.attach(observe, 'texit')
jit.on()
lib.arm64_aggregate_reset()
local function has_native_aggregate_root(xload_type)
  for no in pairs(roots()) do
    local info = jitutil.traceinfo(no)
    if info then
      local callxs, xload = false, false
      for ref = 1, info.nins do
        local _, ot = jitutil.traceir(no, ref)
        if ot then
          local op = bit.rshift(ot, 8)
          local name = vmdef.irnames:sub(op*6+1, op*6+6)
          callxs = callxs or name == 'CALLXS'
          xload = xload or
            (name == 'XLOAD ' and bit.band(ot, 255) == xload_type)
        end
      end
      if callxs and xload then return true end
    end
  end
  return false
end
jit.off(has_native_aggregate_root, true)
local small_cases = {
  { lib.arm64_take_byte, byte, 5, 16 },
  { lib.arm64_take_half, half, 6, 18 },
  { lib.arm64_take_word, word, 7, 20 },
}
for _, case in ipairs(small_cases) do
  jit.flush(candidate)
  exits = 0
  for pass = 1, 8 do
    assert(candidate(16, case[1], case[2]) ==
           oracle(16, case[1], case[2]))
  end
  assert(has_native_aggregate_root(case[4]),
    'small aggregate has no native CALLXS/XLOAD width-matched root')
  assert(exits > 0, 'small aggregate trace did not return through native code')
  assert(lib.arm64_aggregate_count(case[3]) == 256,
    'small aggregate effects were lost or replayed: ' ..
    tostring(lib.arm64_aggregate_count(case[3])))
end
jit.flush(candidate)
for turn = 1, 24 do
  local n = turn % 2 == 0 and 48 or 64
  local use = turn < 12 and fn or alt
  local got = candidate(n, use, pair)
  local want = oracle(n, use, pair)
  assert(got == want, 'native aggregate call disagrees with interpreter')
end
local native_before_gc = _aggregate_native_width_count(8)
collectgarbage('collect')
exits = 0
assert(candidate(32, fn, pair) == oracle(32, fn, pair))
assert(has_native_aggregate_root(22),
  'pair aggregate CALLXS root was not retained after GC')
assert(exits > 0, 'pair aggregate trace did not exit after GC reuse')
assert(_aggregate_native_width_count(8) > native_before_gc,
  'pair aggregate did not re-enter native code after GC')
jit.attach(observe)

local candidate_roots = roots()
for no in pairs(candidate_roots) do
  local info = jitutil.traceinfo(no)
  if info then
    local count = 0
    for ref = 1, info.nins do
      local _, ot = jitutil.traceir(no, ref)
      if ot then
        local pos = 6*bit.rshift(ot, 8)
        if vmdef.irnames:sub(pos+1, pos+6) == 'CALLXS' then
          count = count + 1
        end
      end
    end
    if count > 0 then selected[no] = count end
  end
end
assert(next(selected), 'no selected candidate root contains CALLXS')
assert(exits > 0, 'selected candidate root did not take a native exit')
assert(lib.arm64_aggregate_count(0) == 1312,
  'integer aggregate effects were lost or replayed: ' ..
  tostring(lib.arm64_aggregate_count(0)))
assert(lib.arm64_aggregate_count(1) == 1440,
  'alternate-signature effects were lost or replayed: ' ..
  tostring(lib.arm64_aggregate_count(1)))

assert(stop:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result, 'live peer did not stop and join')
