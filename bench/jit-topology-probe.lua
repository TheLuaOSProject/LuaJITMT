-- Compare a few trace topologies with an interpreter oracle on each target.
-- No TRACE observer is installed: it suppresses ARM64 side publication.
-- Usage: LUA_PATH='src/?.lua;;' src/luajit bench/jit-topology-probe.lua
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')
assert(jit.status(), 'JIT must be enabled')
jit.off(true, true)

local cases = {
  {
    name = 'hot-function-root',
    source = [[return function(x) return x*.5+1.25 end]],
    phases = {{11.5, 17.25}, {'bad'}},
  },
  {
    name = 'nonleaf-function-root-return',
    source = [[local function child(x) return x*.25 end
      return function(x) local y=child(x); return y+.75 end]],
    phases = {{11.5, 17.25}, {'bad'}},
  },
  {
    name = 'two-exit-loop-sides',
    source = [[return function(mode,n)
      local sum=0
      for i=1,n do
        if mode==1 then sum=sum+i*.25
        elseif mode==2 then sum=sum-i*.5
        else sum=sum+i*.75 end
      end
      return sum
    end]],
    phases = {{1,48}, {2,48}, {3,48}, {2,64}},
  },
}

local function compile(source, oracle)
  local chunk=assert(loadstring(source, '@topology-probe'))
  -- Captured sibling helpers belong to the loader's prototype tree.
  -- Keep them interpreted along with the returned oracle runner.
  if oracle then jit.off(chunk,true) end
  return chunk()
end

local function root_headers(fn)
  local roots = {}
  for pc=0,util.funcinfo(fn).bytecodes-1 do
    local ins = util.funcbc(fn,pc)
    if ins then
      local op = bit.band(ins,255)
      local name = vmdef.bcnames:sub(op*6+1,op*6+6)
      if name == 'JFUNCF' or name == 'JFORL ' or name == 'JLOOP ' or
         name == 'JITERL' then
        local no = bit.rshift(ins,16)
        roots[no]=true
      end
    end
  end
  return roots
end

local function trace_rows()
  local rows = {}
  for no=1,256 do
    local info=util.traceinfo(no)
    if info then
      rows[no]=info
    end
  end
  return rows
end

local function describe(rows)
  local out={}
  for no,info in pairs(rows) do
    local fields={}
    for k,v in pairs(info) do
      if type(v) ~= 'function' then fields[#fields+1]=k..'='..tostring(v) end
    end
    table.sort(fields)
    out[#out+1]=tostring(no)..'{'..table.concat(fields,',')..'}'
  end
  table.sort(out)
  return table.concat(out,';')
end

local function id_list(set)
  local ids={}
  for no in pairs(set) do ids[#ids+1]=no end
  table.sort(ids)
  for i=1,#ids do ids[i]=tostring(ids[i]) end
  return table.concat(ids,',')
end

local function side_ids(rows, roots)
  local sides={}
  for no,info in pairs(rows) do
    if info.linktype == 'root' and roots[info.link] then sides[no]=true end
  end
  return sides
end

local function equal(a,b)
  if type(a) ~= type(b) then return false end
  if type(a) == 'number' and a ~= a and b ~= b then return true end
  return a == b
end

local function invoke_pair(candidate, oracle, args)
  local a,b
  if args[1] == 'bad' then
    a={pcall(candidate, unpack(args))}
    b={pcall(oracle, unpack(args))}
  else
    a={true,candidate(unpack(args))}
    b={true,oracle(unpack(args))}
  end
  assert(a[1] == b[1], 'candidate/oracle success differs')
  if not a[1] then
    assert(tostring(a[2]) == tostring(b[2]), 'candidate/oracle error differs')
    return
  end
  assert(#a == #b, 'candidate/oracle result count differs')
  for i=2,#a do
    assert(equal(a[i],b[i]), 'candidate/oracle result differs')
  end
end

local function main()
  local ready,stop=threading.channel(1),threading.channel(1)
  local peer=assert(threading.spawn(function(r,s)
    assert(r:send(true,5)); assert(select(2,s:recv(120))); return true
  end,ready,stop))
  local function exit_observer(no, exitno)
    observed[#observed+1]={no,exitno}
  end
  jit.off(exit_observer,true)
  assert(select(2,ready:recv(5)), 'MT peer did not start')
  io.write('case\tarch\tphase\troot_ids\tpublished_root_linked_sides\tobserved_root_exits\tobserved_other_exits\texit_counts\ttraces\n')
  for _,case in ipairs(cases) do
    jit.flush()
    local candidate=compile(case.source, false)
    local oracle=compile(case.source, true)
    jit.off(oracle,true)
    observed={}
    local known_roots={}
    jit.opt.start('hotloop=1','hotexit=1','maxtrace=128')
    jit.attach(exit_observer,'texit')
    jit.on()
    for phase,args in ipairs(case.phases) do
      for _=1,80 do invoke_pair(candidate,oracle,args) end
      collectgarbage('collect')
      invoke_pair(candidate,oracle,args)
      local roots=root_headers(candidate)
      for no in pairs(roots) do known_roots[no]=true end
      local rows=trace_rows()
      local sides=side_ids(rows,known_roots)
      local root_exits,other_exits=0,0
      local counts={}
      for _,event in ipairs(observed) do
        if known_roots[event[1]] then root_exits=root_exits+1
        else other_exits=other_exits+1 end
        local key=event[1]..':'..event[2]
        counts[key]=(counts[key] or 0)+1
      end
      local events={}
      for key,count in pairs(counts) do events[#events+1]=key..'='..count end
      table.sort(events)
      io.write(string.format('%s\t%s\t%d\t%s\t%s\t%d\t%d\t%s\t%s\n',
        case.name,jit.arch,phase,id_list(roots),id_list(sides),
        root_exits,other_exits,table.concat(events,','),describe(rows)))
      observed={}
    end
    jit.attach(exit_observer)
  end
  assert(stop:send(true,5))
  local joined,result=peer:join(5)
  assert(joined and result, 'MT peer failed to stop')
end
jit.off(compile,true)
jit.off(root_headers,true)
jit.off(trace_rows,true)
jit.off(describe,true)
jit.off(equal,true)
jit.off(invoke_pair,true)
jit.off(main,true)
main()
