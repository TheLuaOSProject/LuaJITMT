-- Existing-key and no-op nil deletion under the active shared runtime.
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

local function loop_delete(t, k, n)
  local i = 1
  while i <= n do t[k] = nil; i = i + 1 end
  return i
end
local function for_delete(t, k, n)
  -- FORL enters native code after its first interpreted body iteration.
  -- Preserve the seeded key until the first generated iteration can delete it.
  for i = 1, n do if i > 1 then t[k] = nil end end
  return n + 1
end

local function run(fn, name, initially_present, k)
  local t = {}
  if k == nil then k = 'rooted-delete-key' end
  local box = { payload = true }
  jit.flush()
  jit.on(fn, true)
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')
  local traces, exits, aborts = {}, 0, {}
  local helper_snapshots, helper_exits = {}, 0
  local function trace(what, no, func, _, reason, detail)
    if what == 'start' and func == fn then traces[no] = true end
    if what == 'abort' and traces[no] then
      traces[no] = nil
      aborts[#aborts+1] = tostring(reason)..':'..tostring(detail)
    end
  end
  local function exitcb(no, sn)
    if traces[no] then
      exits = exits + 1
      if helper_snapshots[no] and helper_snapshots[no][sn] then
        helper_exits = helper_exits + 1
      end
    end
  end
  jit.attach(trace, 'trace')
  jit.attach(exitcb, 'texit')
  for i=1,20 do
    if initially_present then rawset(t, k, box) end
    assert(fn(t, k, 32) == 33 and t[k] == nil)
  end
  local warm_exits = exits
  local native, effects = 0, 0
  for no in pairs(traces) do
    local info = util.traceinfo(no)
    if info then
      native = native + 1
      helper_snapshots[no] = {}
      for ref=1,info.nins do
        local _, op, _, id = util.traceir(no, ref)
        if op then
          local opcode = bit.rshift(op,8)
          local opname = vmdef.irnames:sub(opcode*6+1,opcode*6+6)
          assert(opname ~= 'ASTORE' and opname ~= 'HSTORE' and opname ~= 'FSTORE', name)
          if opname == 'CALLS ' and vmdef.ircall[id] == 'lj_tab_storenil_rooted' then
            effects = effects + 1
            local selected
            for sn=0,info.nexit-1 do
              if util.tracesnap(no, sn)[0] <= ref+1 then selected = sn end
            end
            helper_snapshots[no][assert(selected)] = true
          end
        end
      end
    end
  end
  assert(native == 1 and #aborts == 0,
    name..': native nil-store admission failed: '..table.concat(aborts, ','))
  assert(effects > 0, name..': no rooted nil effect')
  assert(warm_exits >= (initially_present and 10 or 19) and warm_exits <= 21,
    name..': unexpected exit count '..warm_exits)
  assert(t[k] == nil, name..': deletion/no-op result')
  collectgarbage('collect')
  local before = exits
  for i=1,20 do
    t[k] = box -- native reentry must handle post-recording insertion.
    assert(fn(t, k, 32) == 33 and t[k] == nil,
      name..': reentry did not delete inserted key')
  end
  -- Root entry can be refused independently of a helper effect. Require
  -- actual native reentry across the batch, with no deletion-status fallback.
  assert(exits > before and helper_exits == 0,
    name..': post-GC batch did not execute native deletion')
  local calls = 0
  setmetatable(t, { __newindex = function(target, key, value)
    calls = calls + 1; rawset(target, key, value)
  end })
  -- k can still own a retained nil-valued hash node. It is semantically absent,
  -- so this invocation must exit through the helper and invoke __newindex.
  before = helper_exits
  local assignments = (fn == for_delete or rooted_nil_forl_admission) and 1 or 2
  for i=1,20 do assert(fn(t, k, 2) == 3) end
  assert(calls == assignments*20 and helper_exits > before and t[k] == nil,
    name..': nil-valued hash node calls/exits '..calls..'/'..(helper_exits-before))
  setmetatable(t, nil)
  assert(fn(t, k, 32) == 33 and t[k] == nil)
  setmetatable(t, { __newindex = function(target, key, value)
    calls = calls + 1; rawset(target, key, value)
  end })
  calls = 0
  rawset(t, k, 4)
  -- FORL performs one delete; LOOP performs a delete then an absent
  -- assignment, which must invoke __newindex exactly once.
  assert(fn(t, k, 2) == 3)
  assert(calls == assignments-1 and t[k] == nil,
    name..': existing-key delete changed __newindex semantics')
  jit.attach(trace); jit.attach(exitcb)
  print(string.format('rooted nil store %s native=%d effects=%d exits=%d', name, native, effects, warm_exits))
end

jit.off(run, true)
if rooted_nil_store_admission then
  local fn = rooted_nil_forl_admission and for_delete or loop_delete
  -- The C fixture needs immutable, unpatched bytecode for its semantic copy.
  rooted_nil_store_trace_clone = assert(loadstring(string.dump(fn)))
  local kind = rooted_nil_forl_admission and 'FORL' or 'LOOP'
  run(fn, kind..' clone', true)
else
  run(loop_delete, 'LOOP absent', false)
  run(loop_delete, 'LOOP present', true)
  run(for_delete, 'FORL absent', false)
  run(for_delete, 'FORL present', true)
  local keys = {
    {'integer', 17}, {'number', 17.25}, {'true', true}, {'false', false},
    {'table', {}}, {'function', function() end},
    {'thread', coroutine.create(function() end)},
    {'userdata', newproxy(true)}, {'cdata', ffi.new('int[1]', 31)},
  }
  if rooted_nil_lightud then
    keys[#keys+1] = {'lightuserdata', rooted_nil_lightud}
  end
  for _, pair in ipairs(keys) do
    run(loop_delete, 'LOOP key/'..pair[1], true, pair[2])
    run(for_delete, 'FORL key/'..pair[1], true, pair[2])
  end
end

assert(release:send(true,5))
local joined, result = peer:join(5)
assert(joined and result)
print('t-arm64-jit-rooted-nil-stores OK')
