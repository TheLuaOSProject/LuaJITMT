local bit = require("bit")
local threading = require("threading")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

local value = "upvalue"
local function update(new)
  value = new
end

local function upvalue_loop(n)
  local sum = 0
  for _ = 1, n do sum = sum + #value end
  return sum
end

local function cell_loop(cell, n)
  local function update_cell(new)
    cell = new
  end
  local sum = 0
  for _ = 1, n do sum = sum + #cell end
  return sum, update_cell
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

local function native_loop_calls(fn, callee)
  local tr = assert(native_root(fn), "upvalue loop did not compile")
  local in_loop = false
  for ref = 1, util.traceinfo(tr).nins do
    local _, ot, _, id = util.traceir(tr, ref)
    local op = bit.rshift(ot, 8)
    local name = vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
    if name == "LOOP  " then in_loop = true end
    if in_loop and name == "CALLS " and vmdef.ircall[id] == callee then
      return tr
    end
  end
  error("native loop omitted " .. callee)
end
jit.off(native_loop_calls)

local control, ready = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(control, ready, update)
  assert(ready:send(true, 10))
  local replacement, received = control:recv(20)
  assert(received)
  if replacement then
    update(replacement)
    assert(ready:send(true, 10))
  end
  return true
end, control, ready, update))

local resumed = false
local function check()
  assert(select(2, ready:recv(10)))
  jit.off(upvalue_loop)
  jit.off(cell_loop)
  assert(upvalue_loop(128) == 128 * #"upvalue")
  assert(cell_loop("cell", 128) == 128 * #"cell")
  jit.on(upvalue_loop)
  jit.on(cell_loop)
  jit.opt.start("hotloop=2", "hotexit=2")
  for _ = 1, 4 do
    assert(upvalue_loop(128) == 128 * #"upvalue")
    assert(cell_loop("cell", 128) == 128 * #"cell")
  end
  local uv = native_loop_calls(upvalue_loop, "lj_func_getuv_rooted_try")
  native_loop_calls(cell_loop, "lj_func_getcell_rooted_try")

  assert(control:send("replacement", 10))
  resumed = true
  assert(select(2, ready:recv(10)))
  assert(native_root(upvalue_loop) == uv, "publication retired the test trace")
  assert(upvalue_loop(128) == 128 * #"replacement")
end
jit.off(check)

local ok, err = xpcall(check, debug.traceback)
if not resumed then assert(control:send(false, 10)) end
local joined, result = peer:join(10)
assert(ok, err)
assert(joined and result)
print("t-x64-jit-rooted-upvalue ok")
