-- GC-valued table reads and existing-key stores use rooted runtime effects.
-- Each loop keeps a loaded value live across further helper calls and then
-- publishes it through another rooted table operation.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')
local ffi = require('ffi')

assert(jit.arch == 'arm64' and jit.status())
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  local _, ok = rel:recv(120)
  assert(ok)
  return true
end, ready, release)
assert(select(2, ready:recv(5)))

local values = {
  { 'table', {} },
  { 'string', ('dynamic-' .. tostring({})):sub(1, 16) },
  { 'function', function() return 17 end },
  { 'thread', coroutine.create(function() return 23 end) },
  { 'cdata', ffi.new('int[1]', 29) },
}
if type(newproxy) == 'function' then
  values[#values+1] = { 'userdata', newproxy(true) }
end

local function make_case(value)
  local root = { child = {}, sink = {}, noise = {}, key = 'payload' }
  root.child[root.key] = value
  root.sink[root.key] = value
  root.noise[root.key] = 1
  return root
end

local function kernel(root, n)
  local i, matched = 1, 0
  while i <= n do
    local child = root.child
    local key = root.key
    local value = child[key]
    local noise = root.noise[key]
    if noise then matched = matched + 1 end
    root.sink[key] = value
    i = i + 1
  end
  return matched
end

local function pressure_kernel(root, n, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19, p20, p21, p22, p23, p24, p25, p26, p27, p28, p29, p30, p31, p32)
  local i, total = 1, 0
  while i <= n do
    local child = root.child
    local key = root.key
    local value = child[key]
    local noise = root.noise[key]
    total = total + noise
    root.sink[key] = p1
    root.sink[key] = p2
    root.sink[key] = p3
    root.sink[key] = p4
    root.sink[key] = p5
    root.sink[key] = p6
    root.sink[key] = p7
    root.sink[key] = p8
    root.sink[key] = p9
    root.sink[key] = p10
    root.sink[key] = p11
    root.sink[key] = p12
    root.sink[key] = p13
    root.sink[key] = p14
    root.sink[key] = p15
    root.sink[key] = p16
    root.sink[key] = p17
    root.sink[key] = p18
    root.sink[key] = p19
    root.sink[key] = p20
    root.sink[key] = p21
    root.sink[key] = p22
    root.sink[key] = p23
    root.sink[key] = p24
    root.sink[key] = p25
    root.sink[key] = p26
    root.sink[key] = p27
    root.sink[key] = p28
    root.sink[key] = p29
    root.sink[key] = p30
    root.sink[key] = p31
    root.sink[key] = p32
    root.sink[key] = value
    i = i + 1
  end
  return total
end


local function run(label, value)
  local root = make_case(value)
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
  for _ = 1, 20 do assert(kernel(root, 32) == 32, label) end
  jit.attach(trace)
  jit.attach(exit)

  local native, reads, stores = 0, 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local opcode = bit.rshift(op, 8)
          local opname = vmdef.irnames:sub(opcode*6+1, opcode*6+6)
          assert(opname ~= 'ALOAD ' and opname ~= 'HLOAD ' and
                 opname ~= 'ASTORE' and opname ~= 'HSTORE', label)
          if opname == 'CALLS ' then
            local name = vmdef.ircall[id]
            if name == 'lj_tab_gettv_rooted_hit_try' then reads = reads + 1 end
            if name == 'lj_tab_storegc_rooted' then stores = stores + 1 end
          end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
         label .. ': native admission failed: ' .. table.concat(aborts, ','))
  assert(reads >= 3 and stores >= 1, label .. ': missing rooted GC effects')
  assert(exits >= 19 and exits <= 21, label .. ': native trace did not repeat')
  assert(root.sink[root.key] == value, label .. ': stored identity changed')
  collectgarbage('collect')
  assert(kernel(root, 32) == 32, label .. ': post-GC execution')
  _G.__arm64_rooted_gc_reentry = function() return kernel(root, 32) end
  print(string.format('rooted GC operations: %s reads=%d stores=%d exits=%d',
                      label, reads, stores, exits))
end

local function run_pressure()
  local root = make_case(values[1][2])
  local args = {}
  for i = 1, 32 do
    args[i] = { slot = i }
  end
  -- All destinations use one existing key. Keep every argument object live
  -- through the rooted read and later stores so allocated GC spills matter.
  root.key = 'payload'
  root.sink[root.key] = values[1][2]
  jit.flush()
  jit.on(pressure_kernel, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local traces, exits, aborts = {}, 0, {}
  local function trace(what, no, func, _, reason, detail)
    if what == 'start' and func == pressure_kernel then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(reason) .. ':' .. tostring(detail)
    end
  end
  local function exit(no) if traces[no] then exits = exits + 1 end end
  jit.attach(trace, 'trace')
  jit.attach(exit, 'texit')
  for _ = 1, 20 do
    assert(pressure_kernel(root, 32, unpack(args)) == 32)
  end
  jit.attach(trace)
  jit.attach(exit)
  local native, reads, stores = 0, 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      for ref = 1, info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local opcode = bit.rshift(op, 8)
          local opname = vmdef.irnames:sub(opcode*6+1, opcode*6+6)
          if opname == 'CALLS ' then
            local name = vmdef.ircall[id]
            if name == 'lj_tab_gettv_rooted_hit_try' then reads = reads + 1 end
            if name == 'lj_tab_storegc_rooted' then stores = stores + 1 end
          end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
         'GC pressure trace admission failed: ' .. table.concat(aborts, ','))
  assert(reads >= 2 and stores >= 32, 'GC pressure helpers missing')
  assert(exits >= 19 and exits <= 21, 'GC pressure trace did not repeat')
  collectgarbage('collect')
  for _ = 1, 2 do assert(pressure_kernel(root, 32, unpack(args)) == 32) end
  _G.__arm64_rooted_gc_reentry = function()
    return pressure_kernel(root, 32, unpack(args))
  end
  print(string.format('rooted GC pressure: reads=%d stores=%d exits=%d',
                      reads, stores, exits))
end
jit.off(run_pressure, true)


jit.off(run, true)
if rooted_gc_admission then
  rooted_gc_trace_clone = assert(loadstring(string.dump(pressure_kernel)))
  run_pressure()
else
  for _, item in ipairs(values) do run(item[1], item[2]) end
  run_pressure()
end

assert(release:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-gc-operations OK')
