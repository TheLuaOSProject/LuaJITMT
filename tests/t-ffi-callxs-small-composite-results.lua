local ffi = require('ffi')
local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
jit.off(true, true)

if (jit.arch ~= 'x64' and jit.arch ~= 'arm64') or jit.os == 'Windows' then
  print('t-ffi-callxs-small-composite-results SKIP: Darwin ARM64 or SysV x64 only')
  return
end
if not os.getenv('LJ_ARM64_INT_AGGREGATE_SO') then
  print('t-ffi-callxs-small-composite-results SKIP: C helper library not configured')
  return
end
ffi.cdef[[
typedef struct { struct { uint16_t lo, hi; } half; uint32_t tag; } SmallNested;
typedef struct { uint32_t tag; float value; } SmallMixed;
SmallNested arm64_return_nested_int(uint32_t);
SmallMixed arm64_return_mixed(uint32_t);
uint32_t arm64_return_scalar(uint32_t);
void arm64_aggregate_reset(void);
uint32_t arm64_aggregate_count(uint32_t);
]]
local lib = ffi.load(assert(os.getenv('LJ_ARM64_INT_AGGREGATE_SO')))
local cases = {
  {'nested', 'SmallNested', lib.arm64_return_nested_int, 67,
   function(v, n)
     return v.half.lo == n+1 and v.half.hi == n+2 and v.tag == n+3
   end},
  {'mixed', 'SmallMixed', lib.arm64_return_mixed, 68,
   function(v, n) return v.tag == n+1 and v.value == n+.5 end},
}

local function source()
  return assert(loadstring([[
return function(n, fn)
  local sum, last = .5
  for i = 1, n do
    last = fn(i)
    sum = sum+i
  end
  return sum, last
end
]], '@small-composite-result-peer'))()
end

local function loop_trace(fn)
  for pc = 1, util.funcinfo(fn).bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    local op = bit.band(ins, 255)
    if vmdef.bcnames:sub(op*6+1, op*6+6) == 'JFORL ' then
      local id = bit.rshift(ins, 16)
      local info = util.traceinfo(id)
      if info and info.link == id and info.linktype == 'loop' then return id end
    end
  end
end

local function has_callxs(id)
  for ref = 1, util.traceinfo(id).nins do
    local _, typedop = util.traceir(id, ref)
    if typedop then
      local pos = 6*bit.rshift(typedop, 8)
      if vmdef.irnames:sub(pos+1, pos+6) == 'CALLXS' then return true end
    end
  end
  return false
end

local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(120)))
  return true
end, ready, release)
assert(select(2, ready:recv(5)))
for _, case in ipairs(cases) do
  local candidate, oracle = source(), source()
  jit.off(oracle, true)
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.on(candidate, true)
  lib.arm64_aggregate_reset()
  local function compare(n)
    local a, ar = candidate(n, case[3])
    local b, br = oracle(n, case[3])
    assert(a == b and a == .5+n*(n+1)/2)
    assert(ffi.istype(case[2], ar) and ffi.istype(case[2], br))
    assert(case[5](ar, n) and case[5](br, n))
  end
  jit.off(compare, true)
  for pass = 1, 8 do compare(64) end
  assert(lib.arm64_aggregate_count(case[4]) == 1024)
  local root = assert(loop_trace(candidate), 'result loop did not publish')
  assert(has_callxs(root), 'selected result root has no CALLXS')
  local code, address, loop = util.tracemc(root)
  assert(code and address and loop)
  local exits = 0
  local function onexit(no) if no == root then exits = exits+1 end end
  jit.off(onexit, true)
  jit.attach(onexit, 'texit')
  collectgarbage('collect')
  compare(32)
  assert(exits > 0, 'selected result root did not exit after GC')
  assert(lib.arm64_aggregate_count(case[4]) == 1088)
  local gc_exits = exits
  exits = 0
  local a, ar = candidate(32, lib.arm64_return_scalar)
  local b, br = oracle(32, lib.arm64_return_scalar)
  assert(a == b and ar == br and type(ar) == 'number')
  assert(exits > 0, 'alternate result callee did not exit selected root')
  local code_after, address_after, loop_after = util.tracemc(root)
  assert(loop_trace(candidate) == root and has_callxs(root))
  assert(code == code_after and address == address_after and loop == loop_after,
         'selected result root changed across GC or alternate callee')
  jit.attach(onexit)
  print('t-ffi-callxs-small-composite-results OK:', jit.arch, case[1],
        'trace', root, 'post-GC-exits', gc_exits, 'alternate-exits', exits)
end
assert(release:send(true, 5))
local ok, result = peer:join(5)
assert(ok and result, 'live MT peer did not join')
