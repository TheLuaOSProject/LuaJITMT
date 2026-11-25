local bit = require("bit")
local threading = require("threading")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

if jit.arch ~= "x64" or jit.os ~= "Linux" then
  print("t-x64-jit-boxed-arithmetic SKIP")
  return
end
require("ffi")

local function add(n, acc, rhs)
  for _ = 1, n do acc = acc + rhs end
  return acc
end

local function sub(n, acc, rhs)
  for _ = 1, n do acc = acc - rhs end
  return acc
end

local function mul(n, acc, rhs)
  for _ = 1, n do acc = acc * rhs end
  return acc
end

local function native_root(fn)
  for pc = 1, util.funcinfo(fn).bytecodes - 1 do
    local ins = util.funcbc(fn, pc)
    local op = bit.band(ins, 255)
    if vmdef.bcnames:sub(op * 6 + 1, op * 6 + 6) == "JFORL " then
      local tr = bit.rshift(ins, 16)
      if util.traceinfo(tr) then return tr end
    end
  end
end
jit.off(native_root)

local control, ready = threading.channel(1), threading.channel(1)
local mt = assert(debug.getmetatable(1LL))
local original_add = mt.__add
local peer = assert(threading.spawn(function(control, ready, mt)
  assert(ready:send(true, 10))
  while true do
    local mutate, received = control:recv(20)
    assert(received)
    if not mutate then return true end
    if type(mutate) == "number" then threading.sleep(mutate) end
    mt.__add = function(a) return a end
    assert(ready:send(true, 10))
  end
end, control, ready, mt))

local function check()
  assert(select(2, ready:recv(10)))
  jit.flush()
  jit.opt.start("hotloop=2", "hotexit=2")

  local signed = 9223372036854775807LL
  local unsigned = 18446744073709551615ULL
  for _ = 1, 6 do
    assert(tostring(add(48, signed, 1LL)) == "-9223372036854775761LL")
    assert(tostring(sub(32, 1LL, 1ULL)) == "18446744073709551585ULL")
    assert(tostring(mul(64, unsigned, 3ULL)) == "9713657961997484799ULL")
  end
  local root = assert(native_root(add), "boxed addition did not compile")
  assert(native_root(sub), "boxed subtraction did not compile")
  assert(native_root(mul), "boxed multiplication did not compile")

  collectgarbage("collect")
  -- Use the same root with a different CTypeID to exercise its side exits.
  assert(tostring(add(48, 5ULL, 2ULL)) == "101ULL")
  assert(control:send(true, 10))
  assert(select(2, ready:recv(10)))
  assert(native_root(add) == root, "method-slot mutation retired the test root")
  assert(tostring(add(48, 5LL, 2LL)) == "5LL", "stale __add specialization")
  mt.__add = original_add
  assert(tostring(add(48, 5LL, 2LL)) == "101LL")

  -- Require one invocation to observe both handlers. A scheduling delay
  -- can put a mutation entirely before or after an invocation, so retry.
  local n, mixed = 400000, false
  for _ = 1, 3 do
    assert(control:send(0.01, 10))
    local got = tonumber(add(n, 0LL, 1LL))
    assert(select(2, ready:recv(10)))
    assert(native_root(add) == root)
    mt.__add = original_add
    assert(got >= 0 and got <= n)
    if got > 0 and got < n then
      mixed = true
      break
    end
  end
  assert(mixed, "no invocation overlapped the peer's method mutation")
end
jit.off(check)

local ok, err = xpcall(check, debug.traceback)
assert(control:send(false, 10))
local joined, result = peer:join(10)
mt.__add = original_add
assert(ok, err)
assert(joined and result)
print("t-x64-jit-boxed-arithmetic ok")
