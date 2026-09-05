local ffi = require('ffi')
local util = require('jit.util')
ffi.cdef('int abs(int);')
local lib = ffi.C
local mt = debug.getmetatable(lib)
local oldindex = mt.__index
local abs = lib.abs
local calls, exits = 0, 0
local function onexit() exits = exits + 1 end
local function run(n)
  local sum = 0
  for i = 0, n do
    if i > 0 and lib.abs then sum = sum + 1 end
  end
  return sum
end
local function replacement(_, key)
  assert(key == 'abs')
  calls = calls + 1
  return false
end
local function check()
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.attach(onexit, 'texit')
  assert(run(80) == 80)
  local warmexits = exits
  if jit.status() then assert(warmexits > 0, 'warm code must execute natively') end
  mt.__index = replacement
  calls, exits = 0, 0
  local result = run(80)
  mt.__index = oldindex
  jit.attach(onexit)
  print('clib mutable __index', 'warmexits', warmexits, 'result', result, 'calls', calls, 'exits', exits)
  assert(result == 0, 'native code used the old namespace method')
  assert(calls == 80, 'native code skipped the replacement namespace method')
end
jit.off(onexit, true)
jit.off(check, true)
local ok, err = pcall(check)
mt.__index = oldindex
assert(ok, err)
