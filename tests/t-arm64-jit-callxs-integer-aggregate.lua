local ffi = require('ffi')
local jitutil = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')

jit.off(true, true)
ffi.cdef[[
typedef struct { uint32_t lo, hi; } Arm64IntPair;
typedef struct { float lo, hi; } Arm64FloatPair;
typedef Arm64FloatPair& Arm64FloatPairRef;
typedef uint32_t (*Arm64FloatPairVarargFn)(Arm64FloatPair, ...);
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
double arm64_take_float_pair_double_salt(Arm64FloatPair, double);
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
_G.__arm64_float_pair_double_salt_fn = lib.arm64_take_float_pair_double_salt
_G.__arm64_float_vararg_fn = ffi.cast('Arm64FloatPairVarargFn',
                                      lib.arm64_take_float_pair)
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

local hfa_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-candidate'))()
local hfa_ref_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-reference-candidate'))()
local hfa_oracle = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-oracle'))()
local hfa_clone = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-clone'))()
jit.off(hfa_oracle, true)
jit.off(hfa_clone, true)
jit.off(hfa_ref_candidate, true)
_G.__arm64_float_hfa_candidate = hfa_candidate
_G.__arm64_float_hfa_oracle = hfa_oracle
_G.__arm64_float_hfa_clone = hfa_clone
_set_hfa_candidate(hfa_candidate)
jit.on(hfa_ref_candidate, true)

local pair = ffi.new('Arm64IntPair', 7, 11)
local byte = ffi.new('Arm64Byte', 7)
local half = ffi.new('Arm64Half', 701)
local word = ffi.new('Arm64Word', 70001)
local fn = lib.arm64_take_int_pair
local alt = lib.arm64_take_int_pair_double_salt
local function roots(fn)
  fn = fn or candidate
  local found = {}
  for pc = 0, jitutil.funcinfo(fn).bytecodes-1 do
    local ins = jitutil.funcbc(fn, pc)
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
local function hfa_roots()
  return roots(hfa_candidate)
end
jit.off(hfa_roots, true)
local selected, exits, hfa_exits = {}, 0, 0
local function observe(no, exitno)
  if roots()[no] then exits = exits + 1 end
  if hfa_roots()[no] then hfa_exits = hfa_exits + 1 end
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
jit.on(hfa_candidate, true)
jit.on(candidate, true)
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
  { lib.arm64_take_byte, byte, 6, 16 },
  { lib.arm64_take_half, half, 7, 18 },
  { lib.arm64_take_word, word, 8, 20 },
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
jit.flush(hfa_candidate)
local normal_hfa_candidate = hfa_candidate
hfa_candidate = hfa_ref_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
local hfa_ref_seed = ffi.new('Arm64FloatPair', 1.25, 2.5)
local hfa_ref = ffi.cast('Arm64FloatPairRef', hfa_ref_seed)
lib.arm64_aggregate_reset()
for pass = 1, 8 do
  assert(hfa_candidate(16, lib.arm64_take_float_pair_double_salt,
                       hfa_ref, 3.75) ==
         hfa_oracle(16, lib.arm64_take_float_pair_double_salt,
                    hfa_ref, 3.75))
end
assert(_hfa_trace_count() == 0,
  'reference-valued HFA argument published a native root')
assert(lib.arm64_aggregate_count(3) == 256,
  'reference-valued HFA fallback effects were lost or replayed')

hfa_candidate = normal_hfa_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
jit.flush(hfa_candidate)
exits = 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local hfa_seed = ffi.new('Arm64FloatPair', 1.25, 2.5)
local hfa_fn = lib.arm64_take_float_pair_double_salt
for pass = 1, 8 do
  assert(hfa_candidate(16, hfa_fn, hfa_seed, 3.75) ==
         hfa_oracle(16, hfa_fn, hfa_seed, 3.75))
end
assert(hfa_exits > 0, 'HFA root did not take a native exit')
assert(_hfa_trace_count() > 0, 'HFA native root was not published')
assert(_hfa_native_count() > 0, 'HFA root did not finish a native call')
assert(lib.arm64_aggregate_count(3) == 256,
  'HFA warm native/interpreter effects were lost or replayed')
local hfa_before_gc = _hfa_native_count()
collectgarbage('collect')
exits = 0
hfa_exits = 0
local hfa_pair = ffi.new('Arm64FloatPair', 1.25, 2.5)
assert(hfa_candidate(32, lib.arm64_take_float_pair_double_salt,
                     hfa_pair, 3.75) ==
       hfa_oracle(32, lib.arm64_take_float_pair_double_salt,
                  hfa_pair, 3.75))
assert(_hfa_trace_count() > 0, 'HFA root was lost after full GC')
assert(_hfa_native_count() > hfa_before_gc,
  'HFA root did not re-enter native code after full GC')
assert(hfa_exits > 0, 'HFA root did not exit after full-GC re-entry')
assert(lib.arm64_aggregate_count(3) == 320,
  'HFA post-GC effects were lost or replayed')
_set_aggregate_candidate(candidate)
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
