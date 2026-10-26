local jit = require('jit')
local threading = require('threading')
assert(jit.arch == 'arm64' and jit.status())

jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=4')

local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  local _, ok = rel:recv(120)
  assert(ok)
  return true
end, ready, release)
assert(select(2, ready:recv(5)))

-- Retain the certified Lua caller below the root when C invokes a kernel.
function __arm64_rawset_invoke(fn, rawsetfn, t, base, value, n)
  local result = fn(rawsetfn, t, base, value, n)
  return result
end
jit.off(__arm64_rawset_invoke, true)

-- The first form emits a zero-result CALL (B=1); the second consumes the
-- rawset result and emits a one-result CALL (B=2). Keep rawset as an argument
-- so the JIT must guard the exact fast-function identity.
function __arm64_rawset_zero(rawsetfn, t, base, value, n)
  local i = 1
  while i <= n do
    rawsetfn(t, base + i, value)
    i = i + 1
  end
  return i
end

function __arm64_rawset_one_for(rawsetfn, t, base, value, n)
  for i = 1, n do
    local result = rawsetfn(t, base + i, value)
    if result ~= t then return -1 end
  end
  return n + 1
end

-- A successful rooted write is followed by a guarded numeric read. The C
-- fixture changes the committed slot to boolean in the post-CAS hook, forcing
-- an exit after the rawset call has completed. The interpreter must resume at
-- the read and raise the arithmetic error without replaying rawset.
function __arm64_rawset_then_read(rawsetfn, t, base, value, n)
  local i = 1
  while i <= n do
    rawsetfn(t, base + i, value)
    local x = t[base + i] + 1
    i = i + x - value
  end
  return i
end

function __arm64_rawset_release()
  assert(release:send(true, 5))
  local ok, result = peer:join(5)
  assert(ok and result)
end

print('t-arm64-jit-rawset-exits setup OK')
