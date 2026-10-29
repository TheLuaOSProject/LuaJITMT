-- Ordinary-runtime probe for a side trace whose immediate parent is another
-- side trace. No TRACE observer is installed: it would suppress publication.
local jit = require("jit")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

assert(jit.status())

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
local phases = { {1,1}, {2,1}, {2,2} }
local root, first_side
for phase, args in ipairs(phases) do
  for _ = 1, 100 do
    local actual = candidate(args[1], args[2], 48)
    local expected = oracle(args[1], args[2], 48)
    assert(actual == expected, "candidate/oracle result mismatch")
  end
  collectgarbage("collect")
  assert(candidate(args[1], args[2], 48) == oracle(args[1], args[2], 48))
  local rows = traces()
  if phase == 1 then
    for no, row in pairs(rows) do
      if row.parent == 0 and row.info.linktype == "loop" then root = no end
    end
    assert(root, "root loop trace not observed")
  elseif phase == 2 then
    local sides = ids_with_parent(rows, root)
    assert(#sides > 0, "first root-linked child not observed")
    first_side = sides[1]
  end
  local nested = first_side and ids_with_parent(rows, first_side) or {}
  if phase == 3 then
    assert(#nested > 0, "nested child trace not observed")
    for _, no in ipairs(nested) do
      local info = rows[no].info
      assert(info.linktype == "root" and info.link == root,
             "nested child must keep the canonical root final link")
    end
  end
  io.write(string.format(
    "phase=%d arch=%s inputs=%d,%d root=%s first_side=%s nested=%s exits=%s\n",
    phase, jit.arch, args[1], args[2], tostring(root),
    tostring(first_side), csv(nested), exit_csv()))
  exits = {}
end

jit.attach(onexit)
