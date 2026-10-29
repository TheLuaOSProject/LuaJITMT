-- Ordinary-runtime ancestry probe for multiple levels of side traces.
-- IR_BASE.op1 is the immediate recording parent; linktype/link is reported
-- separately because it names the final target, not parentage.
local jit = require("jit")
local util = require("jit.util")
local vmdef = require("jit.vmdef")
local bit = require("bit")
local threading = require("threading")

assert(jit.status())
jit.off(true, true)
local source = [[return function(a,b,c,d,n)
  local s=.5
  for i=1,n do
    if a==1 then
      if b==1 then
        if c==1 then
          if d==1 then s=s+i else s=s+i*.125 end
        else
          if d==1 then s=s+i*.25 else s=s+i*.0625 end
        end
      else
        if c==1 then
          if d==1 then s=s+i*.5 else s=s+i*.03125 end
        else
          if d==1 then s=s+i*.125 else s=s+i*.015625 end
        end
      end
    else
      if b==1 then
        if c==1 then
          if d==1 then s=s-i else s=s-i*.125 end
        else
          if d==1 then s=s-i*.25 else s=s-i*.0625 end
        end
      else
        if c==1 then
          if d==1 then s=s-i*.5 else s=s-i*.03125 end
        else
          if d==1 then s=s-i*.125 else s=s-i*.015625 end
        end
      end
    end
  end
  return s
end]]
local candidate = assert(loadstring(source, "@side-depth-candidate"))()
local oracle_chunk = assert(loadstring(source, "@side-depth-oracle"))
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

local function opname(name, value, width)
  return name:sub(value * width + 1, value * width + width)
end
local function base_parent(no, info)
  for ref=0,info.nins-1 do
    local _,ot,op1,op2=util.traceir(no,ref)
    if opname(vmdef.irnames, math.floor(ot/256), 6) == "BASE  " then
      return op1,op2,ref
    end
  end
end
local function traces()
  local rows={}
  for no=1,128 do
    local info=util.traceinfo(no)
    if info then
      local parent,exit,baseref=base_parent(no,info)
      rows[no]={info=info,parent=parent,exit=exit,baseref=baseref}
    end
  end
  return rows
end
local function candidate_rootid()
  local info=util.funcinfo(candidate)
  local found
  for pc=0,info.bytecodes-1 do
    local ins=util.funcbc(candidate,pc)
    if ins and opname(vmdef.bcnames,bit.band(ins,255),6)=="JFORL " then
      assert(not found,"multiple root loops")
      found=bit.rshift(ins,16)
    end
  end
  return found
end
local function mcode_address(no)
  for _=1,32 do
    local _,addr=util.tracemc(no)
    if addr then return addr end
  end
  error("mcode read unavailable for trace "..no)
end
local function path(rows,no)
  local out,seen={},{}
  while no and no~=0 do
    assert(not seen[no],"trace ancestry cycle")
    seen[no]=true
    local row=assert(rows[no],"missing ancestry trace "..no)
    out[#out+1]=no
    no=row.parent
  end
  return out
end
local function csv(t)
  local out={}
  for i,v in ipairs(t) do out[i]=tostring(v) end
  return table.concat(out,"/")
end
local function describe(rows,ids,mcode)
  local out={}
  for i,no in ipairs(ids) do
    local row=rows[no]
    local base="root"
    if row.parent~=0 then
      base=string.format("IR_BASE@%s=%s:%s", tostring(row.baseref),
                         tostring(row.parent), tostring(row.exit))
    end
    out[i]=string.format("%d@%s[%s;link=%s:%s]",no,tostring(mcode[no]),base,
                         row.info.linktype,tostring(row.info.link))
  end
  return table.concat(out," ")
end
local function exit_csv()
  local keys,out={},{}
  for key in pairs(exits) do keys[#keys+1]=key end
  table.sort(keys)
  for i,key in ipairs(keys) do out[i]=key.."="..exits[key] end
  return table.concat(out,",")
end

local ready,stop=threading.channel(1),threading.channel(1)
local peer=assert(threading.spawn(function(r,s)
  assert(r:send(true,5)); assert(select(2,s:recv(5000))); return true
end,ready,stop))
assert(select(2,ready:recv(5)),"MT peer did not start")
local phases={ {1,1,1,1}, {2,1,1,1}, {2,2,1,1}, {2,2,2,1} }
local root,ids,mcode
for phase,args in ipairs(phases) do
  for _=1,100 do
    local got=candidate(args[1],args[2],args[3],args[4],48)
    local want=oracle(args[1],args[2],args[3],args[4],48)
    assert(got==want,"candidate/oracle mismatch")
  end
  local warm_exits=exit_csv()
  collectgarbage("collect")
  exits={}
  assert(candidate(args[1],args[2],args[3],args[4],48) ==
         oracle(args[1],args[2],args[3],args[4],48), "post-GC result mismatch")
  local postgc_exits=exit_csv()
  local rows=traces()
  root=root or candidate_rootid()
  assert(root and rows[root] and rows[root].parent==0 and
         rows[root].info.linktype=="loop","candidate root missing")
  ids=ids or {root}
  mcode=mcode or {[root]=mcode_address(root)}
  -- At each stage, retain the deepest trace in the chain and locate a newly
  -- recorded direct child by its exact IR_BASE parent.
  local parent=ids[#ids]
  local children={}
  for no,row in pairs(rows) do
    if row.parent==parent and row.info.linktype=="root" and
       row.info.link==root then children[#children+1]=no end
  end
  table.sort(children)
  if phase>1 and #children>0 then
    local child=children[1]
    if ids[#ids]~=child then ids[#ids+1]=child; mcode[child]=mcode_address(child) end
  end
  -- Prove identities for every remembered edge after the collection.
  rows=traces()
  assert(candidate_rootid()==root and rows[root] and mcode_address(root)==mcode[root])
  for i=2,#ids do
    local no,parentno=ids[i],ids[i-1]
    assert(rows[no] and rows[no].parent==parentno and
           rows[no].info.linktype=="root" and rows[no].info.link==root,
           "remembered immediate-parent/root-link edge changed")
    assert(mcode_address(no)==mcode[no],"mcode identity changed")
  end
  local chain=path(rows,ids[#ids])
  io.write(string.format("phase=%d arch=%s inputs=%s ids=%s chain=%s traces={%s} warm_exits=%s postgc_exits=%s\n",
    phase,jit.arch,csv(args),csv(ids),csv(chain),describe(rows,ids,mcode),warm_exits,postgc_exits))
  exits={}
end

-- Reenter the deepest observed trace after GC on its selected source path.
collectgarbage("collect")
local timed_cases={
  {"root",1,1,1,1}, {"direct",2,1,1,1},
  {"nested",2,2,1,1}, {"depth3",2,2,2,1}
}
jit.opt.start("hotexit=1000000")
for _,case in ipairs(timed_cases) do
  local result
  for _=1,16 do result=candidate(case[2],case[3],case[4],case[5],48) end
  local start=os.clock()
  for _=1,2000 do result=candidate(case[2],case[3],case[4],case[5],48) end
  local elapsed=os.clock()-start
  assert(result==oracle(case[2],case[3],case[4],case[5],48),"timed result mismatch")
  io.write(string.format("timing=%s arch=%s calls=2000 seconds_per_call=%.9f\n",
    case[1],jit.arch,elapsed/2000))
end
for _=1,32 do assert(candidate(2,2,2,2,48)==oracle(2,2,2,2,48)) end
local rows=traces()
assert(candidate_rootid()==root and mcode_address(root)==mcode[root])
for i=2,#ids do
  local no,parentno=ids[i],ids[i-1]
  assert(rows[no] and rows[no].parent==parentno and
         rows[no].info.linktype=="root" and rows[no].info.link==root and
         mcode_address(no)==mcode[no])
end
local native=0
for key,count in pairs(exits) do
  local no=tonumber(key:match("^(%d+):"))
  if no==ids[#ids] then native=native+count end
end
io.write(string.format("phase=reentry arch=%s depth=%d deepest_native_exits=%d traces={%s} exits=%s\n",
  jit.arch,#ids-1,native,describe(rows,ids,mcode),exit_csv()))
jit.attach(onexit)
assert(stop:send(true,5))
local joined,result=peer:join(5)
assert(joined and result,"MT peer failed to stop")
