-- Native ARM64 contract for the immutable GCstr length field load.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')

assert(jit and jit.status(), 'JIT is required')
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')

string_len_trace = function(s, n)
  local i, sum = 1, 0
  while i <= n do
    sum = sum + #s
    i = i + 1
  end
  return sum
end

-- The admission fixture clones this function's source to make an independent
-- recorder certificate. Keep its bytecode body identical to string_len_trace.
string_len_trace_clone = function(s, n)
  local i, sum = 1, 0
  while i <= n do
    sum = sum + #s
    i = i + 1
  end
  return sum
end

local traced, native_exits = {}, 0
local function trace_event(what, traceno, func)
  if what == 'start' and func == string_len_trace then
    traced[traceno] = true
  elseif what == 'abort' then
    traced[traceno] = nil
  end
end
local function trace_exit(traceno)
  if traced[traceno] then native_exits = native_exits + 1 end
end
jit.attach(trace_event, 'trace')
jit.attach(trace_exit, 'texit')

local s = string.rep('str-len-', 32)
assert(string_len_trace(s, 80) == #s * 80)
local function has_length()
  local traceno = next(traced)
  if not traceno then return false end
  local info = util.traceinfo(traceno)
  if not info then return false end
  for ref = 1, info.nins do
    local _, typedop, _, field = util.traceir(traceno, ref)
    if typedop then
      local op = bit.rshift(typedop, 8)
      local name = vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
      if name == 'FLOAD ' and vmdef.irfield[field] == 'str.len' then
        return true
      end
    end
  end
  return false
end
jit.off(has_length, true)
assert(has_length(), 'string loop did not retain a native GCstr length load')

-- The argument remains rooted while a full collection runs; reentry must use
-- the same live trace and return the exact byte length.
collectgarbage('collect')
local before = native_exits
assert(string_len_trace(s, 80) == #s * 80)

-- A table argument must leave the string-specialized trace and keep Lua's
-- ordinary table length semantics.
assert(string_len_trace({1, 2, 3, 4}, 80) == 320)
assert(native_exits > before, 'wrong-type input did not exit the string trace')

jit.attach(trace_event)
jit.attach(trace_exit)
print('t-arm64-jit-string-len OK')
