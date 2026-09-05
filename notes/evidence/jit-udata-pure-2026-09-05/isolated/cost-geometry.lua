local ffi = require("ffi")
local util = require("jit.util")
local vmdef = require("jit.vmdef")
local bit = require("bit")
local kind = assert(arg[1])
local n = tonumber(arg[2]) or (kind == "call" and 500000 or 20000000)
local obj, member, close
ffi.cdef("int abs(int);")
if kind == "file" then
  obj = assert(io.tmpfile()); member = "write"; close = obj.close
else
  obj, member = ffi.C, "abs"
end
assert(obj[member])
local function lookup(count)
  local sum = 0
  for i = 1, count do if obj[member] then sum = sum + 1 end end
  return sum
end
local function foreign(count)
  local sum = 0
  for i = 1, count do sum = sum + obj.abs(-i) end
  return sum
end
local run = kind == "call" and foreign or lookup
local exits = {}
local function onexit(tr) exits[tr] = (exits[tr] or 0) + 1 end
local function main()
  jit.flush(); jit.opt.start("hotloop=1", "hotexit=1000")
  jit.attach(onexit, "texit")
  local value = run(80)
  assert(value == (kind == "call" and 3240 or 80))
  local native, callxs, nins, mcbytes = 0, 0, 0, 0
  for tr=1,200 do
    local info = util.traceinfo(tr)
    if info then
      native = native + (exits[tr] or 0)
      nins = nins + info.nins
      local mc = util.tracemc(tr)
      mcbytes = mcbytes + #mc
      for ref=1,info.nins do
        local _, ot = util.traceir(tr, ref)
        if ot then
          local op = vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
          if op == "CALLXS" then callxs = callxs + 1 end
        end
      end
    end
  end
  assert(native > 0, "timed loop must have executed natively")
  if kind == "call" then assert(callxs > 0, "foreign benchmark must use actual CALLXS") end
  jit.attach(onexit)
  if kind == "file" then
    local open = assert(package.loadlib(assert(arg[3]), "luaopen_hash_geometry"))
    local measure = open()
    print("geometry", measure(obj, member))
  end
  local best = math.huge
  local expected = kind == "call" and n*(n+1)/2 or n
  for pass=1,5 do
    local start = os.clock()
    local result = run(n)
    local seconds = os.clock()-start
    assert(result == expected)
    best = math.min(best, seconds)
    print("sample", pass, string.format("%.9f", seconds), result)
  end
  print("result", kind, n, string.format("%.9f", best), nins, mcbytes, native, callxs)
end
jit.off(onexit, true); jit.off(main, true)
main()
if close then close(obj) end
