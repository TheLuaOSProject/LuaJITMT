-- Ordinary-runtime probe for a side trace whose immediate parent is another
-- side trace. No TRACE observer is installed: it would suppress publication.
local jit = require("jit")
local util = require("jit.util")
local vmdef = require("jit.vmdef")
local bit = require("bit")
local threading = require("threading")

assert(jit.status())
jit.off(true, true)

local source = [[return function(outer,inner,n)
  local s=.5
  for i=1,n do
    if outer==1 then
      if inner==1 then s=s+i else s=s+i*.25 end
    else
      if inner==1 then s=s-i else s=s-i*.5 end
    end
  end
  return s
end]]
local candidate = assert(loadstring(source, "@side-of-side-candidate"))()
local oracle_chunk = assert(loadstring(source, "@side-of-side-oracle"))
jit.off(oracle_chunk, true)
local oracle = oracle_chunk()
local exits = {}
local function onexit(trace, exit)
  local key = trace .. ":" .. exit
  exits[key] = (exits[key] or 0) + 1
end
jit.off(onexit, true)
jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=128")
jit.attach(onexit, "texit")

local function base_parent(no, info)
  for ref = 0, info.nins - 1 do
    local _, ot, op1 = util.traceir(no, ref)
    local opname = vmdef.irnames:sub(math.floor(ot / 256) * 6 + 1,
                                     math.floor(ot / 256) * 6 + 6)
    if opname == "BASE  " then return op1 end
  end
end

local function traces()
  local rows = {}
  for no = 1, 128 do
    local info = util.traceinfo(no)
    if info then
      rows[no] = { info = info, parent = base_parent(no, info) }
    end
  end
  return rows
end

local function ids_with_parent(rows, parent)
  local ids = {}
  for no, row in pairs(rows) do
    if row.parent == parent and row.info.linktype == "root" then
      ids[#ids + 1] = no
    end
  end
  table.sort(ids)
  return ids
end

local function candidate_rootid()
  local info = util.funcinfo(candidate)
  local found
  for pc = 0, info.bytecodes-1 do
    local ins = util.funcbc(candidate, pc)
    if ins then
      local op = bit.band(ins,255)
      local name = vmdef.bcnames:sub(op*6+1,op*6+6)
      if name == "JFORL " then
        local no = bit.rshift(ins,16)
        assert(not found, "more than one candidate root loop")
        found = no
      end
    end
  end
  return found
end

local function mcode_address(no)
  local addr
  for _ = 1, 32 do
    local _, current = util.tracemc(no)
    if current then addr = current; break end
  end
  assert(addr, "trace mcode read unavailable for trace " .. no)
  return addr
end

local root, first_side
local nested_ids = {}
local saved_mcode = {}
local function assert_live_graph(rows)
  rows = rows or traces()
  assert(candidate_rootid() == root, "candidate JFORL root changed")
  assert(rows[root] and rows[root].parent == 0 and
         rows[root].info.linktype == "loop",
         "candidate root trace was retired or reused")
  assert(mcode_address(root) == saved_mcode[root],
         "candidate root mcode identity changed")
  if first_side then
    assert(rows[first_side] and rows[first_side].parent == root and
           rows[first_side].info.linktype == "root" and
           rows[first_side].info.link == root,
           "direct side trace was retired or reused")
    assert(mcode_address(first_side) == saved_mcode[first_side],
           "direct side mcode identity changed")
  end
  for _, no in ipairs(nested_ids) do
    assert(rows[no] and rows[no].parent == first_side and
           rows[no].info.linktype == "root" and
           rows[no].info.link == root,
           "nested side trace was retired or reused")
    assert(mcode_address(no) == saved_mcode[no],
           "nested side mcode identity changed")
  end
  return rows
end

local function csv(t)
  local out = {}
  for i, value in ipairs(t) do out[i] = tostring(value) end
  return table.concat(out, ",")
end

local function exit_csv()
  local keys = {}
  for key in pairs(exits) do keys[#keys + 1] = key end
  table.sort(keys)
  local out = {}
  for i, key in ipairs(keys) do out[i] = key .. "=" .. exits[key] end
  return table.concat(out, ",")
end

jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=128")
local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r,s)
  assert(r:send(true,5)); assert(select(2,s:recv(120))); return true
end, ready, stop))
assert(select(2,ready:recv(5)), "MT peer did not start")
local phases = { {1,1}, {2,1}, {2,2} }
for phase, args in ipairs(phases) do
  for _ = 1, 100 do
    local actual = candidate(args[1], args[2], 48)
    local expected = oracle(args[1], args[2], 48)
    assert(actual == expected, "candidate/oracle result mismatch")
    if phase == 3 and #ids_with_parent(traces(), first_side) > 0 then
      -- Preserve a terminal exit as an ordinary native witness once the
      -- nested edge exists. This changes recording policy, not admission.
      jit.opt.start("hotexit=1000")
      break
    end
  end
  collectgarbage("collect")
  assert(candidate(args[1], args[2], 48) == oracle(args[1], args[2], 48))
  local rows = traces()
  if phase == 1 then
    root = candidate_rootid()
    assert(root, "root loop trace not observed")
    assert(rows[root] and rows[root].parent == 0 and
           rows[root].info.linktype == "loop")
    saved_mcode[root] = mcode_address(root)
  elseif phase == 2 then
    local sides = ids_with_parent(rows, root)
    assert(#sides > 0, "first root-linked child not observed")
    first_side = sides[1]
    saved_mcode[first_side] = mcode_address(first_side)
  end
  local nested = first_side and ids_with_parent(rows, first_side) or {}
  if phase == 3 then
    assert(#nested > 0, "nested child trace not observed")
    nested_ids = nested
    for _, no in ipairs(nested) do
      local info = rows[no].info
      assert(info.linktype == "root" and info.link == root,
             "nested child must keep the canonical root final link")
      saved_mcode[no] = mcode_address(no)
    end
  end
  assert_live_graph(rows)
  io.write(string.format(
    "phase=%d arch=%s inputs=%d,%d root=%s first_side=%s nested=%s exits=%s\n",
    phase, jit.arch, args[1], args[2], tostring(root),
    tostring(first_side), csv(nested), exit_csv()))
  exits = {}
end

-- Reenter the already-published graph after another full collection. A
-- matching Lua result alone would miss root-entry rejection and fallback.
exits = {}
collectgarbage("collect")
assert_live_graph()
for _ = 1, 32 do
  assert(candidate(2,2,48) == oracle(2,2,48))
end
assert_live_graph()
local native_nested = 0
for _, no in ipairs(nested_ids) do
  for key, count in pairs(exits) do
    if tonumber(key:match("^(%d+):")) == no then
      native_nested = native_nested+count
    end
  end
end
assert(native_nested > 0, "nested child did not execute after GC and publication")
io.write(string.format("phase=reentry arch=%s nested_native_exits=%d exits=%s\n",
  jit.arch, native_nested, exit_csv()))
jit.attach(onexit)
assert(stop:send(true,5))
local joined, result = peer:join(5)
assert(joined and result, "MT peer failed to stop")
