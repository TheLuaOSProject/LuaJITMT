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
]]

local function trace_op_count(wanted)
  local count = 0
  for tr = 1, 128 do
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

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
if arm64_scalar then
  assert(ffi.C.fabs(-3.25) == 3.25)
  assert(run_arm64_i32(ffi.C.abs, 400))
  assert(run_arm64_double(ffi.C.fabs, 400))
else
  assert(run(400) == 400 * 401 / 2)
end
local xsave = trace_op_count("XSAVE ")
local callxs = trace_op_count("CALLXS")
if arm64_scalar then
  assert(xsave == 4 and callxs == 4,
         ("wrong ARM64 scalar CALLXS lifecycles: %d/%d"):format(
           xsave, callxs))
  jit.flush()
  assert(run_arm64_double(ffi.C.fabsf, 400))
  assert(trace_op_count("CALLXS") == 0,
         "unsupported float(float) CALLXS trace was published")
else
  assert(xsave > 0, "production scalar FFI call omitted XSAVE")
  assert(callxs > 0, "production scalar FFI call omitted CALLXS")
end

if arm64_scalar then
  print("t-ffi-callxs-production OK: int/double CALLXS executed")
else
  print("t-ffi-callxs-production OK: default scalar CALLXS executed")
end
