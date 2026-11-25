local bit = require("bit")
local threading = require("threading")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

local function run(n, t, nextfn, key)
  local sum = 0
  for _ = 1, n do
    local k, v = nextfn(t, key)
    if k then sum = sum + (v == false and 13 or v) end
  end
  return sum
end

local function run_without_key(n, t, nextfn)
  local sum = 0
  for _ = 1, n do
    local _, v = nextfn(t)
    sum = sum + v
  end
  return sum
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

local function has_next_call(tr)
  local in_loop = false
  for ref = 1, util.traceinfo(tr).nins do
    local _, ot, _, id = util.traceir(tr, ref)
    local op = bit.rshift(ot, 8)
    local name = vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
    if name == "LOOP  " then in_loop = true end
    if in_loop and name == "CALLS " and
       vmdef.ircall[id] == "lj_tab_nextscalar_forjit" then return true end
  end
end
jit.off(has_next_call)

local t = {1, 2, 3, 4, 5, 6, 7, 8}
local control, ready = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(control, ready, t)
  assert(ready:send(true, 10))
  local mutate, received = control:recv(20)
  assert(received)
  if mutate then
    t.extra = 11
    assert(ready:send(true, 10))
  end
  return true
end, control, ready, t))

local mutation_sent = false
local function check()
  assert(select(2, ready:recv(10)))
  jit.flush()
  jit.opt.start("hotloop=2", "hotexit=2")
  for _ = 1, 6 do assert(run(128, t, next, 6) == 896) end
  local root = assert(native_root(run), "array next() did not compile")
  assert(has_next_call(root), "native loop omitted the traversal call")
  for _ = 1, 4 do assert(run_without_key(64, t, next) == 64) end
  assert(has_next_call(assert(native_root(run_without_key))),
    "native loop omitted next() without a key")

  assert(run(128, t, next, 8) == 0, "end of iteration must return nil")
  assert(run(128, t, next, nil) == 128, "nil must start iteration")
  t[7] = false
  assert(run(128, t, next, 6) == 1664, "boolean result type exit")
  t[7] = 7
  collectgarbage("collect")
  assert(run(128, t, next, 6) == 896)
  local ok, err = pcall(run, 128, t, next, 99)
  assert(not ok and err:find("invalid key", 1, true))

  assert(control:send(true, 10))
  mutation_sent = true
  assert(select(2, ready:recv(10)))
  assert(native_root(run) == root, "hash growth retired the test root")
  assert(run(128, t, next, 8) == 1408, "hash growth must refuse the array path")
  assert(run(128, t, next, 6) == 896)
end
jit.off(check)

local ok, err = xpcall(check, debug.traceback)
if not mutation_sent then assert(control:send(false, 10)) end
local joined, result = peer:join(10)
assert(ok, err)
assert(joined and result)
print("t-x64-jit-next-scalar ok")
