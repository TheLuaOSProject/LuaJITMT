local ffi, th = require('ffi'), require('threading')
local util, vmdef, bit = require('jit.util'), require('jit.vmdef'), require('bit')
local kind, so, candidate = assert(arg[1]), assert(arg[2]), arg[3] == 'candidate'
ffi.cdef[[
extern int reg_clib_root_slot;
int reg_clib_root_fn(void);
enum { REG_CLIB_ROOT_ZERO = 0 };
]]
local lib = ffi.load(so)
local index, newindex = debug.getmetatable(lib).__index, debug.getmetatable(lib).__newindex
assert(index(lib, 'reg_clib_root_fn'))
assert(index(lib, 'reg_clib_root_slot') == 17)
assert(index(lib, 'REG_CLIB_ROOT_ZERO') == 0)
local key = kind == 'function' and 'reg_clib_root_fn' or kind == 'number' and 'REG_CLIB_ROOT_ZERO' or 'reg_clib_root_slot'
local expected = debug.getfenv(lib)[key]
local function run_function(fn, receiver, n)
  local sum = 0
  for i = 1, n do if fn(receiver, 'reg_clib_root_fn') then sum = sum + 1 end end
  return sum
end
local function run_read(fn, receiver, n)
  local sum = 0
  for i = 1, n do sum = sum + fn(receiver, 'reg_clib_root_slot') end
  return sum
end
local function run_write(fn, receiver, n)
  local sum = 0
  for i = 1, n do fn(receiver, 'reg_clib_root_slot', i); sum = sum + 1 end
  return sum
end
local function run_number(fn, receiver, n)
  local sum = 0
  for i = 1, n do sum = sum + fn(receiver, 'REG_CLIB_ROOT_ZERO') end
  return sum
end
local run = assert(({['function']=run_function, read=run_read, write=run_write, number=run_number})[kind])
local roots, exits = {}, {}
local function ontrace(what, tr, func, pc, parent)
  if what == 'start' and func == run and (not parent or parent == 0) then roots[tr] = true end
end
local function onexit(tr) exits[tr] = (exits[tr] or 0) + 1 end
local ready, stop = th.channel(1), th.channel(1)
local peer = th.spawn(function(ready, stop)
  assert(ready:send(true))
  local val, ok = stop:recv(60); assert(ok and val)
  return true
end, ready, stop)
assert(select(2, ready:recv(10)))
local function main()
  collectgarbage('collect'); collectgarbage('collect')
  jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.attach(ontrace, 'trace'); jit.attach(onexit, 'texit')
  local result = run(kind == 'write' and newindex or index, lib, 80)
  assert(result == (kind == 'read' and 1360 or kind == 'number' and 0 or 80))
  if kind == 'write' then assert(index(lib, 'reg_clib_root_slot') == 80) end
  local root
  for tr in pairs(roots) do
    local ti = util.traceinfo(tr)
    if ti and ti.link == tr and ti.linktype == 'loop' and (exits[tr] or 0) > 0 then root = tr; break end
  end
  assert(root, 'actual native root required')
  local ti, inloop = util.traceinfo(root), false
  local found, old, kgc, vload, precalls, loopcalls = 0, 0, 0, 0, 0, 0
  for ref = 1, ti.nins do
    local _, ot, a, b = util.traceir(root, ref)
    if ot then
      local op = vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6):gsub('%s+$','')
      if op == 'LOOP' then inloop = true end
      if op == 'VLOAD' then vload = vload + 1 end
      if op == 'CALLS' then
        local name = vmdef.ircall[b]
        if name == 'lj_tab_gettv_rooted_hit_try' then old = old + 1 end
        if name == 'lj_tab_cmpcdata_kgc_rooted_try' then
          found = found + 1
          local _, argot, left, expectedref = util.traceir(root, a)
          assert(vmdef.irnames:sub(6*bit.rshift(argot,8)+1,6*bit.rshift(argot,8)+6) == 'CARG  ')
          assert(expectedref < 0, 'expected cdata is an IR constant')
          local actual = util.tracek(root, expectedref)
          assert(type(actual) == 'cdata' and rawequal(actual, expected), 'actual expected cdata KGC argument')
          kgc = kgc + 1
        end
        if name == 'lj_tab_gettv_rooted_hit_try' or name == 'lj_tab_cmpcdata_kgc_rooted_try' then
          if inloop then loopcalls = loopcalls + 1 else precalls = precalls + 1 end
        end
      end
    end
  end
  assert(precalls == 1 and loopcalls == 1, 'one cache helper per loop iteration')
  if candidate and kind ~= 'number' then assert(found == 2 and kgc == 2 and old == 0 and vload == 0)
  else assert(found == 0 and old == 2 and vload == 2) end
  print('shape', kind, root, ti.nins, #util.tracemc(root), found, old, kgc, vload, precalls, loopcalls, exits[root])
end
jit.off(main, true); jit.off(ontrace, true); jit.off(onexit, true)
local ok, err = pcall(main)
jit.attach(ontrace); jit.attach(onexit)
assert(stop:send(true, 10))
local joined, value = peer:join(10); assert(joined and value)
assert(ok, err)
