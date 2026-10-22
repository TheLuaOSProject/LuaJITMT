-- Special userdata metatables and their __index/__newindex entries are mutable.
-- Re-enter the original native loop after mutation, then check exact Lua effects.
local ffi = require("ffi")
local util = require("jit.util")
local kind, mode = arg[1] or "clib", arg[2] or "function"
local expect_hoisted = arg[3] ~= "baseline"
local vmdef, bit = require("jit.vmdef"), require("bit")
local n, calls, writes = 80, 0, 0
local obj, key, close
if kind == "clib" then
  ffi.cdef("int abs(int);")
  obj, key = ffi.C, "abs"
  assert(obj[key]) -- Populate the real namespace cache before recording.
elseif kind == "file" then
  obj, key = assert(io.tmpfile()), "write"
  close = obj.close
elseif kind == "buffer" then
  obj, key = require("string.buffer").new(), "put"
elseif kind == "plain" then
  obj, key = newproxy(true), "marker"
  debug.getmetatable(obj).__index = {marker = true}
else
  error("unknown userdata kind: " .. tostring(kind))
end
local mt = debug.getmetatable(obj)
local oldindex, oldnewindex = mt.__index, mt.__newindex
local added, exits, oldids = false, {}, {}
local iswrite = mode == "newindex" or mode == "newindex_table"
local methodtable, weakmethod
local readers = {
  clib = function(p,count) local sum=0; for i=0,count do if i>0 and p.abs then sum=sum+1 end end; return sum end,
  file = function(p,count) local sum=0; for i=0,count do if i>0 and p.write then sum=sum+1 end end; return sum end,
  buffer = function(p,count) local sum=0; for i=0,count do if i>0 and p.put then sum=sum+1 end end; return sum end,
  plain = function(p,count) local sum=0; for i=0,count do if i>0 and p.marker then sum=sum+1 end end; return sum end,
}
local run_read = assert(readers[kind])
local function run_write(p,count)
  for i = 0, count do if i > 0 then p[key] = i end end
end
local function shape(ids)
  local root
  for _, id in ipairs(ids) do local ti=util.traceinfo(id);if ti.link==id and ti.linktype=="loop" then root=id;break end end
  assert(root,"original self-linked root")
  local info=util.traceinfo(root)
  local loop,meta,postmeta,nodes,postnodes,polls=0,0,0,0,0,0
  for ref=1,info.nins do
    local _,ot,a,b=util.traceir(root,ref)
    if ot then
      local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
      if op=="LOOP  " then loop=ref end
      if op=="XPOLL " then assert(a==0,"pre-MT mode-zero poll");polls=polls+1 end
      if op=="FLOAD " and vmdef.irfield[b]=="udata.meta" then if loop==0 then meta=meta+1 else postmeta=postmeta+1 end end
      if op=="FLOAD " and vmdef.irfield[b]=="tab.node" then if loop==0 then nodes=nodes+1 else postnodes=postnodes+1 end end
    end
  end
  assert(loop>0 and polls==1 and meta==1 and nodes>=1,"entry meta/node guards and phase poll remain")
  assert(postmeta==(expect_hoisted and 0 or 1),"pure metatable reuse shape")
  assert(postnodes==nodes-(expect_hoisted and 1 or 0),"only the immediate metatable node is reusable")
  print("direct-shape",kind,mode,root,info.nins,meta,nodes,postmeta,postnodes)
end
local function oldwrite() writes = writes + 1 end
local function newwrite() writes = writes + 1000 end
local function replacement(receiver, field)
  assert(receiver == obj and field == key)
  calls = calls + 1
  return false
end
local function make_method()
  return function() return true end
end
local function onexit(trace)
  exits[trace] = (exits[trace] or 0) + 1
end
local function traceids()
  local ids = {}
  for id = 1, 200 do
    if util.traceinfo(id) then ids[#ids + 1] = id end
  end
  return ids
end
local function exitcount(ids)
  local count = 0
  for _, id in ipairs(ids) do count = count + (exits[id] or 0) end
  return count
end
local function check()
  if iswrite then
    mt.__newindex = oldwrite
  elseif mode == "methodlife" then
    mt.__index = make_method()
    weakmethod = setmetatable({mt.__index}, {__mode = "v"})
  elseif mode == "table_entry" then
    methodtable = {[key] = true}
    mt.__index = methodtable
  end
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1000")
  jit.attach(onexit, "texit")
  local run = iswrite and run_write or run_read
  local warm = run(obj,n)
  if iswrite then assert(writes == n, "warm write count")
  else assert(warm == n, "warm read result") end
  oldids = traceids()
  if jit.status() and not iswrite then shape(oldids) end
  local warmexits = exitcount(oldids)
  if jit.status() then
    assert(#oldids > 0 and warmexits > 0, "warm loop must execute native code")
  end
  local expects_error = false
  local expected_calls = 0
  if mode == "function" or mode == "resize" or mode == "methodlife" then
    mt.__index = replacement
    expected_calls = n
    if mode == "resize" then
      added = true
      for i = 1, 256 do mt["special_udata_resize_" .. i] = i end
      collectgarbage("collect")
    elseif mode == "methodlife" then
      collectgarbage("collect")
      if jit.status() then
        assert(weakmethod[1] ~= nil, "trace must retain the old recorded method")
      end
    end
  elseif mode == "table" then
    mt.__index = {[key] = false}
  elseif mode == "missing" then
    mt.__index = nil
    expects_error = true
  elseif mode == "nonfunction" then
    mt.__index = 42
    expects_error = true
  elseif mode == "replace" then
    local replacement_mt = {}
    for k, v in pairs(mt) do replacement_mt[k] = v end
    replacement_mt.__index = replacement
    debug.setmetatable(obj, replacement_mt)
    expected_calls = n
  elseif mode == "replace_missing" then
    debug.setmetatable(obj, nil)
    expects_error = true
  elseif mode == "table_entry" then
    methodtable[key] = false
  elseif mode == "newindex" then
    mt.__newindex = newwrite
  elseif mode == "newindex_table" then
    methodtable = {}
    mt.__newindex = methodtable
  else
    error("unknown mode: " .. tostring(mode))
  end
  if jit.status() then
    for _, id in ipairs(oldids) do
      assert(util.traceinfo(id), "userdata mutation must retain the original trace for its guards")
    end
  end
  calls, writes, exits = 0, 0, {}
  local ok, result = pcall(run, obj,n)
  local oldexits = exitcount(oldids)
  print("special-userdata-guard", kind, mode, "warm-exits", warmexits,
        "old-exits", oldexits, "ok", ok, "result", result,
        "calls", calls, "writes", writes)
  if expects_error then
    assert(not ok, "native code skipped the current absent/nonfunction method")
  else
    assert(ok, result)
    if iswrite then
      if mode == "newindex" then
        assert(writes == 1000*n, "native code skipped the replacement __newindex")
      else
        assert(writes == 0 and methodtable[key] == n, "native code skipped table-valued __newindex")
      end
    else
      assert(result == 0, "native code used the old userdata method")
    end
  end
  assert(calls == expected_calls, "native code skipped the replacement __index")
  if jit.status() then
    assert(oldexits > 0, "post-mutation execution must enter the original native loop")
  end
end
local function cleanup()
  debug.setmetatable(obj, mt)
  mt.__index, mt.__newindex = oldindex, oldnewindex
  if added then
    for i = 1, 256 do mt["special_udata_resize_" .. i] = nil end
  end
  jit.attach(onexit)
  if close then close(obj) end
end
for _, f in ipairs({onexit, traceids, exitcount, check, cleanup, shape}) do jit.off(f, true) end
local ok, err = pcall(check)
cleanup()
assert(ok, err)
print("special userdata native method guards passed")
