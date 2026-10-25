-- Operation contracts must compose independently of a loop's example shape.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')

assert(jit.arch == 'arm64' and jit.status())
local a, b, ints = {}, {}, {}
for i = 1, 64 do a[i], b[i], ints[i] = i + 0.25, i + 0.5, i end
a[0.5] = 7.25
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  local _, ok = rel:recv(120)
  assert(ok)
  return true
end, ready, release)
local _, ok = ready:recv(5)
assert(ok)

local cases = {
  { 'argument key', function(t, k, n)
      local i, sum = 1, 0.5
      while i <= n do sum = sum + t[k]; i = i + 1 end
      return sum
    end, { a, 2, 32 }, 72.5, 2, 0 },
  { 'fractional hash key', function(t, k, n)
      local i, sum = 1, 0.5
      while i <= n do sum = sum + t[k]; i = i + 1 end
      return sum
    end, { a, 0.5, 32 }, 232.5, 2, 0 },
  { 'two tables and computed key', function(t, u, k, n)
      local i, sum = 1, 0.5
      while i <= n do sum = sum + t[k] + u[k+1]; i = i + 1 end
      return sum
    end, { a, b, 2, 32 }, 184.5, 4, 0 },
  { 'induction key', function(t, n)
      local i, sum = 1, 0.5
      while i <= n do sum = sum + t[i]; i = i + 1 end
      return sum
    end, { a, 32 }, 536.5, 2, 0 },
  { 'integer result', function(t, n)
      local i, sum = 1, 0
      while i <= n do sum = sum + t[i]; i = i + 1 end
      return sum
    end, { ints, 32 }, 528, 2, 0 },
  { 'read and length', function(t, n)
      local i, sum = 1, 0.5
      while i <= n do sum = sum + t[i] + #t; i = i + 1 end
      return sum
    end, { a, 32 }, 2584.5, 2, 2 },
  { 'five reads per iteration', function(t, k, n)
      local i, sum = 1, 0.5
      while i <= n do
        sum = sum + t[k] + t[k+1] + t[k+2] + t[k+3] + t[k+4]
        i = i + 1
      end
      return sum
    end, { a, 2, 32 }, 680.5, 10, 0 },
}

local function run(case)
  local name, kernel, args, expected = case[1], case[2], case[3], case[4]
  jit.flush()
  jit.on(kernel, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local traces, exits, aborts = {}, 0, {}
  local function trace(what, no, func, _, reason, detail)
    if what == 'start' and func == kernel then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(reason) .. ':' .. tostring(detail)
    end
  end
  local function exit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace')
  jit.attach(exit, 'texit')
  for i = 1, 20 do assert(kernel(unpack(args)) == expected, name) end
  jit.attach(trace)
  jit.attach(exit)
  local native, reads, lengths = 0, 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local opcode = bit.rshift(op, 8)
          local opname = vmdef.irnames:sub(opcode*6+1, opcode*6+6)
          assert(opname ~= 'ALOAD ' and opname ~= 'HLOAD ', name)
          if opname == 'CALLS ' then
            if vmdef.ircall[id] == 'lj_tab_getscalar_rooted_try' then
              reads = reads + 1
            elseif vmdef.ircall[id] == 'lj_tab_len_rooted_try' then
              lengths = lengths + 1
            end
          end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
         name .. ': native admission failed: ' .. table.concat(aborts, ','))
  assert(exits >= 19 and exits <= 21, name .. ': repeated side exits')
  assert(reads == case[5] and lengths == case[6], name .. ': helper effects')
  collectgarbage('collect')
  assert(kernel(unpack(args)) == expected, name .. ': post-GC reentry')
  print(string.format('rooted operations: %s, traces=%d exits=%d reads=%d lengths=%d',
                      name, native, exits, reads, lengths))
end
jit.off(run, true)
if rooted_operations_admission then
  -- The C corruption fixture rebuilds semantic context against a separate,
  -- byte-for-byte identical prototype while retaining this native trace.
  rooted_read_trace_clone = assert(loadstring(string.dump(cases[3][2])))
  run(cases[3])
else
  for _, case in ipairs(cases) do run(case) end
end
assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-operations OK')
