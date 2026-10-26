local bit = require "bit"
local ffi = require "ffi"
local util = require "jit.util"
local vmdef = require "jit.vmdef"

local arm64_scalar = jit.arch == "arm64" and jit.os == "OSX" and
                     os.getenv("LJ_M7_FFI_CALLXS_ARM64_SCALAR") == "1"
if jit.arch ~= "x64" and not arm64_scalar then
  print("t-ffi-callxs-production SKIP: unsupported lowering")
  return
end

ffi.cdef [[
int abs(int);
double fabs(double);
float fabsf(float);
double fma(double, double, double);
double ldexp(double, int);
]]

local function trace_op_count(wanted)
  local count = 0
  for tr = 1, 1000 do
    local info = util.traceinfo(tr)
    if info then
      for ref = 1, info.nins do
        local _, ot = util.traceir(tr, ref)
        if ot then
          local opidx = bit.rshift(ot, 8)
          local op = vmdef.irnames:sub(opidx * 6 + 1, opidx * 6 + 6)
          if op == wanted then count = count + 1 end
        end
      end
    end
  end
  return count
end
jit.off(trace_op_count, true)

local function run(n)
  local sum = 0
  for i = 1, n do
    sum = sum + ffi.C.abs(-i)
  end
  return sum
end

local function run_arm64_i32(fn, n)
  for i = 1, n do
    if fn(i) ~= i then return false end
  end
  return true
end

local function run_arm64_double(fn, n)
  for i = 1, n do
    if fn(i) ~= i then return false end
  end
  return true
end

local function run_arm64_float(fn, n)
  for i = 1, n do
    local x = i * 0.123456789
    local got, mag = fn(x), x
    if got == mag or got < mag - 0.0001 or got > mag + 0.0001 then
      return false
    end
  end
  return true
end

local function run_arm64_float_int(fn, n)
  for i = 1, n do
    local x = 16777217 + 2 * i
    local got = fn(x)
    if got == x or got < x - 2 or got > x + 2 then return false end
  end
  return true
end

local function run_arm64_float_to_double(ff, fd, n)
  for i = 1, n do
    local x = i * 0.123456789
    local rounded = ff(x)
    local got = fd(rounded)
    if got == x or got < x - 0.0001 or got > x + 0.0001 then
      return false
    end
  end
  return true
end

local function run_arm64_nested_float(ff, fd, n)
  for i = 1, n do
    local x = i * 0.123456789
    local got = fd(ff(x))
    if got == x or got < x - 0.0001 or got > x + 0.0001 then
      return false
    end
  end
  return true
end

local function run_arm64_float_to_float(fn, n)
  for i = 1, n do
    local x = i * 0.123456789
    local rounded = fn(x)
    local got = fn(rounded)
    if got == x or got < x - 0.0001 or got > x + 0.0001 then
      return false
    end
  end
  return true
end

local function run_arm64_fma(fn, n)
  for i = 1, n do
    local a = i * 0.5
    if fn(a, 2.0, -i) ~= 0.0 then return false end
  end
  return true
end

local function run_arm64_mixed(fn, n)
  for i = 1, n do
    if fn(i, 3) ~= i * 8 then return false end
  end
  return true
end

local function callxs_lifecycle_count()
  return trace_op_count("XSAVE "), trace_op_count("CALLXS")
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
if arm64_scalar then
  assert(ffi.C.fabs(-3.25) == 3.25)
  assert(run_arm64_i32(ffi.C.abs, 400))
  assert(run_arm64_double(ffi.C.fabs, 400))
  local base_xsave, base_callxs = callxs_lifecycle_count()
  assert(run_arm64_float(ffi.C.fabsf, 400))
  local num_float_xsave, num_float_callxs = callxs_lifecycle_count()
  assert(num_float_xsave == base_xsave + 2 and
         num_float_callxs == base_callxs + 2,
         "NUM-to-F32 loop did not publish both call lifecycles")
  assert(run_arm64_float_int(ffi.C.fabsf, 400))
  local float_xsave, float_callxs = callxs_lifecycle_count()
  assert(float_xsave == num_float_xsave + 2 and
         float_callxs == num_float_callxs + 2,
         "INT-to-F32 loop did not publish both call lifecycles")
  assert(run_arm64_float_to_double(ffi.C.fabsf, ffi.C.fabs, 400))
  local widened_xsave, widened_callxs = callxs_lifecycle_count()
  assert(widened_xsave == float_xsave + 4 and
         widened_callxs == float_callxs + 4,
         "F32-to-double arguments did not publish both call pairs")
  assert(run_arm64_nested_float(ffi.C.fabsf, ffi.C.fabs, 400))
  local nested_xsave, nested_callxs = callxs_lifecycle_count()
  assert(nested_xsave == widened_xsave + 4 and
         nested_callxs == widened_callxs + 4,
         ("nested CALLM float lifecycles: %d/%d -> %d/%d"):format(
           widened_xsave, widened_callxs, nested_xsave, nested_callxs))
  assert(run_arm64_float_to_float(ffi.C.fabsf, 400))
  local chained_xsave, chained_callxs = callxs_lifecycle_count()
  assert(chained_xsave == nested_xsave + 4 and
         chained_callxs == nested_callxs + 4,
         "F32-to-F32 arguments did not publish both call pairs")
  assert(run_arm64_fma(ffi.C.fma, 400))
  local fma_xsave, fma_callxs = callxs_lifecycle_count()
  assert(run_arm64_mixed(ffi.C.ldexp, 400))
  local all_xsave, all_callxs = callxs_lifecycle_count()
  print("CALLXS profile counts", base_xsave, base_callxs,
        float_xsave, float_callxs, nested_xsave, nested_callxs,
        fma_xsave, fma_callxs, all_xsave, all_callxs)
else
  assert(run(400) == 400 * 401 / 2)
end
local xsave = trace_op_count("XSAVE ")
local callxs = trace_op_count("CALLXS")
if arm64_scalar then
  assert(xsave == 24 and callxs == 24,
         ("wrong ARM64 scalar CALLXS lifecycles: %d/%d"):format(
           xsave, callxs))
else
  assert(xsave > 0, "production scalar FFI call omitted XSAVE")
  assert(callxs > 0, "production scalar FFI call omitted CALLXS")
end

if arm64_scalar then
  print("t-ffi-callxs-production OK: int/float/double CALLXS executed")
else
  print("t-ffi-callxs-production OK: default scalar CALLXS executed")
end
