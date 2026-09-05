local ffi = require('ffi')
ffi.cdef('int abs(int);')
local mode = assert(arg[1])
local lib = mode == 'closed' and ffi.load('c') or ffi.C
local oldabs = lib.abs
local env = debug.getfenv(lib)
local exits = 0
local function onexit() exits = exits + 1 end
local function run(n)
  local sum = 0
  for i=0,n do
    if i > 0 and lib.abs then sum=sum+1 end
  end
  return sum
end
local function check()
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.attach(onexit, 'texit')
  assert(run(80)==80)
  local warmexits = exits
  if jit.status() then assert(warmexits>0) end
  if mode == 'closed' then
    debug.getmetatable(lib).__gc(lib)
  else
    assert(mode == 'env')
    env.abs = false
  end
  exits = 0
  local ok,res = pcall(run,80)
  if mode == 'env' then env.abs=oldabs end
  jit.attach(onexit)
  print('CLibrary cache or lifecycle', mode, 'warmexits', warmexits, 'ok', ok, 'result', res, 'exits', exits)
  if mode == 'closed' then
    assert(not ok, 'native lookup accepted a closed CLibrary')
  else
    assert(ok and res == 0, 'native lookup skipped the changed namespace environment')
  end
end
jit.off(onexit,true)
jit.off(check,true)
check()
