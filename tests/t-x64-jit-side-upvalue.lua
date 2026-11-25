local bit = require("bit")
local threading = require("threading")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

local flat_value = string.rep("flat", 23)
local function update_flat(new)
  flat_value = new
end

local function flat(_, limit, cut, stop)
  local sum = 0
  for i = 1, limit do
    sum = sum + i
    if i >= cut then
      local result = flat_value
      if i == stop then return result, sum, i end
    end
  end
  return nil, sum, limit
end

local function make_reader(value)
  local function read(i, cut, stop)
    if i >= cut then
      local result = value
      if i == stop then return result end
    end
  end
  local function update(new)
    value = new
  end
  jit.off(update)
  return read, update
end

local function nested(read, limit, cut, stop)
  local sum = 0
  for i = 1, limit do
    sum = sum + i
    local result = read(i, cut, stop)
    if result then return result, sum, i end
  end
  return nil, sum, limit
end

jit.off(update_flat)
jit.off(make_reader)
local read, update_nested = make_reader(string.rep("nested", 23))
local alternate = make_reader(string.rep("alternate", 23))
-- Specialize calls by prototype so the side trace must inherit the closure.
for i = 3, 6 do make_reader(i) end

local control, ready = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(control, ready)
  assert(ready:send(true, 10))
  while true do
    local command, received = control:recv(20)
    assert(received)
    if command == false then return true end
    local update, as_table = command[1], command[2]
    if as_table then
      update({marker = string.rep("payload", 23)})
    else
      update(string.rep("replacement", 23))
    end
    assert(ready:send(true, 10))
  end
end, control, ready))

local function opname(ot)
  local op = bit.rshift(ot, 8) * 6
  return vmdef.irnames:sub(op + 1, op + 6)
end

local function check(run, get, arg, update, initial, second)
  local parents, exits = {}, {}
  local function trace(what, tr, fn, _, parent)
    if what == "start" then
      parents[tr] = fn == get and parent or nil
    end
  end
  local function exit(tr, ex)
    local counts = exits[tr]
    if not counts then counts = {}; exits[tr] = counts end
    counts[ex] = (counts[ex] or 0) + 1
  end
  local function result(input)
    local value, sum, i = run(input, 128, 32, 96)
    assert(sum == 4656 and i == 96, "side exit changed loop state")
    return value
  end

  jit.off(run)
  assert(result(arg) == string.rep(initial, 23))
  jit.on(run)
  jit.flush()
  jit.opt.start("hotloop=2", "hotexit=2")
  jit.attach(trace, "trace")
  jit.attach(exit, "texit")
  for _ = 1, 5 do assert(result(arg) == string.rep(initial, 23)) end

  local side, type_exit, later_exit
  for tr, parent in pairs(parents) do
    local info = util.traceinfo(tr)
    if info and info.link == parent and exits[tr] then
      local call
      for ref = 1, info.nins do
        local _, ot, _, operand = util.traceir(tr, ref)
        local op = opname(ot)
        if op == "CALLS " and vmdef.ircall[operand] == "lj_func_getuv_rooted_try" then
          call = ref
        elseif call and op == "VLOAD " then
          local result_exit
          for ex = 0, info.nexit - 1 do
            if util.tracesnap(tr, ex)[0] <= ref then result_exit = ex end
          end
          for ex in pairs(exits[tr]) do
            if ex > result_exit then
              side, type_exit, later_exit = tr, result_exit, ex
              break
            end
          end
          break
        end
      end
    end
  end
  assert(side, "expected a native side exit after the rooted upvalue read")
  assert(util.tracemc(side))

  if second then
    local before = exits[side][later_exit]
    assert(result(second) == string.rep("alternate", 23))
    assert(exits[side][later_exit] > before, "side must read the inherited closure")
  end

  local function replace(as_table)
    assert(control:send({update, as_table}, 10))
    assert(select(2, ready:recv(10)))
    collectgarbage("collect")
    assert(util.traceinfo(side), "collection retired the side trace")
  end

  replace(false)
  local before = exits[side][later_exit]
  assert(result(arg) == string.rep("replacement", 23))
  assert(exits[side][later_exit] > before, "native side must see same-type replacement")

  replace(true)
  before = exits[side][type_exit] or 0
  local payload = result(arg)
  assert(type(payload) == "table" and payload.marker == string.rep("payload", 23))
  assert((exits[side][type_exit] or 0) > before, "side must exit on the changed type")

  jit.attach(trace)
  jit.attach(exit)
end
jit.off(check, true)
jit.off(opname)

local function main()
  assert(select(2, ready:recv(10)))
  check(flat, flat, nil, update_flat, "flat")
  check(nested, read, read, update_nested, "nested", alternate)
end
jit.off(main)

local ok, err = xpcall(main, debug.traceback)
assert(control:send(false, 10))
local joined, result = peer:join(10)
assert(ok, err)
assert(joined and result)
print("t-x64-jit-side-upvalue ok")
