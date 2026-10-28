-- Semantic and IR-shape companion for bounded native string byte reads.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')

assert(jit.status(), 'JIT is required')
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')

string_byte_trace = function(s, n)
  local byte = string.byte
  local i, sum = 1, 0
  while i <= n do sum = sum + byte(s, 2); i = i + 1 end
  return sum
end

local traced = {}
local function trace_event(what, traceno, fn)
  if what == 'start' and fn == string_byte_trace then
    traced[traceno] = true
  elseif what == 'abort' then
    traced[traceno] = nil
  end
end
jit.attach(trace_event, 'trace')

local s = string.rep('gcstr-byte-', 24)
assert(string_byte_trace(s, 80) == string.byte('gcstr-byte-', 2) * 80)

local function has_bounded_byte_load()
  local traceno = next(traced)
  if not traceno then return false end
  local info = util.traceinfo(traceno)
  if not info then return false end
  local have_len, have_ref, have_byte = false, false, false
  for ref = 1, info.nins do
    local _, typedop = util.traceir(traceno, ref)
    if typedop then
      local op = bit.rshift(typedop, 8)
      local name = vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
      if name == 'FLOAD ' then have_len = true end
      if name == 'STRREF' then have_ref = true end
      if name == 'XLOAD ' then have_byte = true end
    end
  end
  return have_len and have_ref and have_byte
end
jit.off(has_bounded_byte_load, true)
assert(has_bounded_byte_load(), 'trace omitted bounded string byte IR')

collectgarbage('collect')
assert(string_byte_trace(s, 80) == 99 * 80)
assert(string_byte_trace('a\0b', 80) == 0)
assert(string_byte_trace(string.rep('L', 65536), 80) == 76 * 80)
assert(string.byte('', 2) == nil)
assert(string.byte('x', 2) == nil)
assert(not pcall(string_byte_trace, '', 80))
assert(not pcall(string_byte_trace, {1, 2, 3}, 80))

jit.attach(trace_event)
print('t-arm64-jit-string-byte OK')
