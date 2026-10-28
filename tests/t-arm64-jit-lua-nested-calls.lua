-- Ordinary-runtime checks for a captured nested callee. The native fixture
-- owns native-entry and exact root-exit claims.

local threading = require('threading')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
assert(jit.status(), 'JIT must be enabled')
jit.off(true, true)

local function trace_info_retry(id)
  for attempt = 1, 64 do
    local info = util.traceinfo(id)
    if info then return info end
    -- traceinfo returns no values while another TG owns the JIT read lease.
    threading.sleep(0)
  end
  return nil
end
jit.off(trace_info_retry, true)

local function loop_trace(fn)
  local fi = util.funcinfo(fn)
  if not fi or not fi.bytecodes then return nil end
  for pc = 1, fi.bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      if vmdef.bcnames:sub(op*6+1, op*6+6) == 'JFORL ' then
        local id = bit.rshift(ins, 16)
        local ti = trace_info_retry(id)
        if ti and ti.linktype == 'loop' and ti.link == id then return id end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local function make_pair(label)
  -- Load the candidate prototypes after the broad jit.off(true,true) setup.
  -- f captures g and has the inventory's exact nested-call topology.
  local source = [[
    local g=function(x) return x+0.25 end
    local g2=function(x) return x+0.25 end
    local function set_g(next_g) g=next_g end
    local f=function(x) local v=g(x); return v*0.5 end
    local run=function(n)
      local s=0.5
      for i=1,n do s=s+f(i+0.5) end
      return s
    end
    return g,g2,f,set_g,run
  ]]
  local chunk, err = loadstring(source, '@nested-calls-'..label)
  assert(chunk, err)
  return chunk()
end
jit.off(make_pair, true)

local function main()
  -- Keep a second TG active before compiling and warming the root.
  local ready, release = threading.channel(1), threading.channel(1)
  local peer = assert(threading.spawn(function(rdy, rel)
    assert(rdy:send(true, 5))
    assert(select(2, rel:recv(120)))
    return true
  end, ready, release))
  assert(select(2, ready:recv(5)), 'MT peer did not become ready')

  local g, g2, f, set_g, run = make_pair('candidate')
  local oracle_g, oracle_g2, oracle_f, oracle_set_g, oracle_run =
    make_pair('oracle')
  assert(g ~= g2 and oracle_g ~= oracle_g2 and g ~= oracle_g,
         'callees and oracle closures must have distinct identities')
  -- Explicitly keep every oracle layer interpreted, including both callees.
  jit.off(oracle_g, true)
  jit.off(oracle_g2, true)
  jit.off(oracle_f, true)
  jit.off(oracle_set_g, true)
  jit.off(oracle_run, true)
  jit.off(set_g, true)

  jit.flush()
  jit.on()
  jit.on(g, true)
  jit.on(g2, true)
  jit.on(f, true)
  jit.on(run, true)
  jit.opt.start('hotloop=1', 'hotexit=1000000', 'maxtrace=64')

  local root_trace, exits = nil, 0
  local function texit(no)
    -- No TRACE observer is installed: publication detection must not alter
    -- recording. Count only the exact root selected from its JFORL.
    if root_trace == no then exits = exits+1 end
  end
  jit.off(texit, true)
  jit.attach(texit, 'texit')

  local function check(n, label)
    local expected = oracle_run(n)
    local got = run(n)
    assert(got == expected, label..': result differs from interpreter oracle')
  end
  jit.off(check, true)

  local attempts
  for attempt = 1, 8 do
    attempts = attempt
    check(48, 'cold warmup')
    root_trace = loop_trace(run)
    if root_trace then break end
  end
  assert(root_trace, 'no self-linked JFORL root after 8 cold calls')
  local ti = util.traceinfo(root_trace)
  assert(ti and ti.linktype == 'loop' and ti.link == root_trace,
         'selected root is not a live self-linked loop')

  local function reuse(n, label)
    check(n, label)
    assert(loop_trace(run) == root_trace, label..': selected root changed')
  end
  jit.off(reuse, true)

  reuse(24, 'native root reuse before replacement')
  local before_replace = exits
  set_g(g2)
  reuse(24, 'equivalent callee identity replacement')
  local after_replace = exits
  assert(after_replace > before_replace,
         'callee identity replacement did not exit the selected root')

  set_g(g)
  reuse(24, 'restored callee identity')
  local after_restore = exits
  assert(after_restore > after_replace,
         'restoring the captured callee did not exit the selected root')
  collectgarbage('collect')
  reuse(24, 'full-GC exact root reuse')

  -- Pointwise-equivalent callees make each interleaving agree with the stable
  -- interpreted oracle. The bounded writer mutates only the captured identity.
  local concurrent_ready, concurrent_start = threading.channel(1), threading.channel(1)
  local writer = assert(threading.spawn(function(rdy, go, setter, first_g, second_g)
    assert(rdy:send(true, 5))
    assert(select(2, go:recv(5)))
    for i = 1, 1200 do
      setter(i % 2 == 0 and first_g or second_g)
      if i % 8 == 0 then threading.sleep(0) end
    end
    return true
  end, concurrent_ready, concurrent_start, set_g, g, g2))
  assert(select(2, concurrent_ready:recv(5)), 'writer did not become ready')
  local before_concurrent = exits
  assert(concurrent_start:send(true, 5), 'writer start send failed')
  for i = 1, 160 do reuse(24, 'concurrent equivalent callees '..i) end
  local joined, result = writer:join(20)
  assert(joined and result,
         'concurrent writer did not join cleanly: '..tostring(result))
  local during_concurrent = exits
  assert(during_concurrent > before_concurrent,
         'concurrent captured-callee changes produced no root exits')

  set_g(g)
  reuse(24, 'restored root after concurrent writes')
  assert(loop_trace(run) == root_trace,
         'exact root was not reused after restoration')
  jit.attach(texit)
  assert(release:send(true, 5), 'peer release failed')
  local peer_ok, peer_result = peer:join(5)
  assert(peer_ok and peer_result, 'MT peer did not join cleanly')
  io.write(string.format(
    'lua-nested-calls ordinary OK: attempts=%d trace=%d replacement_exits=%d restore_exits=%d concurrent_exits=%d same_trace_reuse=yes oracle=interpreted\n',
    attempts, root_trace, after_replace-before_replace,
    after_restore-after_replace, during_concurrent-before_concurrent))
end
jit.off(main, true)
main()
