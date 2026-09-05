local ffi = require('ffi')
ffi.cdef('int abs(int);')
local lib = ffi.C
local abs = lib.abs
local index = debug.getmetatable(lib).__index
local exits = 0
local function onexit() exits = exits + 1 end
local function run(receiver, n)
  local sum = 0
  for i=0,n do
    if i > 0 and index(receiver, 'abs') then sum=sum+1 end
  end
  return sum
end
local function check()
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.attach(onexit, 'texit')
  assert(run(lib,80)==80)
  local warmexits = exits
  if jit.status() then assert(warmexits>0) end
  exits = 0
  local ok,res = pcall(run, io.stdout, 80)
  jit.attach(onexit)
  print('captured clib method', 'warmexits', warmexits, 'ok', ok, 'result', res, 'exits', exits)
  assert(not ok, 'captured CLibrary index accepted an IO userdata')
end
jit.off(onexit,true)
jit.off(check,true)
check()
