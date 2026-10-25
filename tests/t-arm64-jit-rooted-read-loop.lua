-- Native ARM64 contract for a fixed-key shared-table numeric read in BC_LOOP.
-- A peer remains live while recording and while the native root runs.
local jit = require('jit')
local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')

assert(jit and jit.status(), 'JIT is required')
local table_value = { 1.25 } -- Construct before MT activation.
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5) == true)
  local _, ok = rel:recv(60)
  assert(ok == true)
  return true
end, ready, release)
local _, ready_ok = ready:recv(5)
assert(ready_ok == true, 'active peer failed to start')

jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=2')
local rooted_traces, exits = {}, 0
rooted_read_trace_clone = function(t, n)
  local i, sum = 1, 0
  while i <= n do
    sum = sum + t[1]
    i = i + 1
  end
  return sum
end

local function read_loop(t, n)
  local i, sum = 1, 0
  while i <= n do
    sum = sum + t[1]
    i = i + 1
  end
  return sum
end
local function trace_event(what, traceno, func)
  if what == 'start' and func == read_loop then
    rooted_traces[traceno] = true
  elseif what == 'abort' then
    rooted_traces[traceno] = nil
  end
end
local function trace_exit(traceno)
  if rooted_traces[traceno] then exits = exits + 1 end
end
jit.attach(trace_event, 'trace')
jit.attach(trace_exit, 'texit')

assert(read_loop(table_value, 64) == 80)
local saw_profile = false
for traceno in pairs(rooted_traces) do
  local info = util.traceinfo(traceno)
  if info then
    local calls, table_roots, key_roots, values, raw_loads = 0, 0, 0, 0, 0
    for ref = 1, info.nins do
      local _, typedop, _, op2 = util.traceir(traceno, ref)
      if typedop then
        local op = bit.rshift(typedop, 8)
        local name = vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
        if name == 'CALLS ' and
           vmdef.ircall[op2] == 'lj_tab_getscalar_rooted_try' then
          calls = calls + 1
        elseif name == 'TMPREF' then
          if op2 == 3 then table_roots = table_roots + 1 end
          if op2 == 8 then key_roots = key_roots + 1 end
        elseif name == 'NE    ' then
          values = values + 1
        elseif name == 'VLOAD ' then
          values = values + 2
        elseif name == 'ALOAD ' or name == 'HLOAD ' then
          raw_loads = raw_loads + 1
        end
      end
    end
    assert(calls == 2 and table_roots == 2 and key_roots == 2 and
           values == 6 and raw_loads == 0,
           'native trace lost the rooted table/key/result ABI')
    saw_profile = true
  end
end
assert(saw_profile, 'rooted read loop did not enter a native trace')
assert(exits > 0, 'rooted native trace did not produce a texit witness')

-- The result root is rewritten by the helper on every iteration, so a later
-- run must observe a new primitive value without flushing the trace.
table_value[1] = 2.5
assert(read_loop(table_value, 64) == 160)
assert(exits > 1, 'updated rooted read did not execute the native trace')

jit.attach(trace_event)
jit.attach(trace_exit)
assert(release:send(true, 5) == true)
local ok, result = peer:join(5)
assert(ok == true and result == true, 'active peer failed to stop')
print('t-arm64-jit-rooted-read-loop OK')
