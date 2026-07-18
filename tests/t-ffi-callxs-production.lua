local bit = require "bit"
local ffi = require "ffi"
local util = require "jit.util"
local vmdef = require "jit.vmdef"

if jit.arch ~= "x64" then
  print("t-ffi-callxs-production SKIP: x64-only lowering")
  return
end

ffi.cdef [[
int abs(int);
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

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(run(400) == 400 * 401 / 2)
assert(trace_op_count("XSAVE ") > 0,
       "production scalar FFI call omitted XSAVE")
assert(trace_op_count("CALLXS") > 0,
       "production scalar FFI call omitted CALLXS")

print("t-ffi-callxs-production OK: default scalar CALLXS executed")
