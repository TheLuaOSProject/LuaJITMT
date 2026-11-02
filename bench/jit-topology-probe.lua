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
    name = 'loop-root-three-child-frame-chain',
    -- FORL root -> top -> middle -> bottom keeps three child calls active
    -- at the deepest return/exit. Each layer has one fixed-arity call and
    -- captures only its immediate child, matching the admitted leaf grammar.
    -- The accumulator keeps the top call non-tail.
    source = [[local function bottom(x) return x+.25 end
      local middle=function(x) return bottom(x)*.5 end
      local top=function(x) return middle(x)-.125 end
      return function(n,offset)
        local sum=.5
        for i=1,n do
          local y=top(offset)
          sum=sum+y+i+.0625
        end
        return sum
      end]],
    phases = {{48,0}, {48,'bad'}},
  },
  {
    name = 'function-root-three-child-frame-chain',
    -- Keep this separate from the FORL-rooted case: it exercises the general
    -- FUNCF caller source with explicit function parameters and the same
    -- three-deep non-tail child chain.
    source = [[local function bottom(x) return x+.25 end
      local function middle(child,x) return child(x)*.5 end
      local function top(child,leaf,x) return child(leaf,x)-.125 end
      return function(topfn,middlefn,bottomfn,x)
        local y=topfn(middlefn,bottomfn,x); return y+.0625
      end, {top,middle,bottom}]],
    helper_args = true,
    phases = {{11.5}, {'bad'}},
  },
  {
    name = 'captured-two-result-call',
    source = [[local function child(x) return x*.5, x+1.25 end
      return function(x) local a,b=child(x); return a+b end]],
    phases = {{11.5, 17.25}, {'bad'}},
  },
  {
    name = 'captured-tail-return',
    source = [[local function leaf(x) return x*.25 end
      return function(x) return leaf(x) end]],
    phases = {{11.5}, {'bad'}},
  },
  {
    name = 'function-two-results',
    source = [[return function(x) return x*.5, x+1.25 end]],
    phases = {{11.5}, {'bad'}},
  },
  {
    name = 'recursive-fibonacci',
    source = [[local function fib(n)
      if n<2 then return n end
      return fib(n-1)+fib(n-2)
      end
      return fib]],
    phases = {{8}, {10}, {'bad'}},
  },
  {
    name = 'protected-leaf',
    source = [[local function leaf(x) return x*.25 end
      return function(x) local ok,y=pcall(leaf,x)
        if ok then return y end; return -1 end]],
    phases = {{11.5}, {'bad'}},
  },
  {
    name = 'vararg-forward-callm-loop',
    source = [[local function sum(a,b,c) return a+b+c end
      local function forward(...) return sum(...) end
      return function(n)
        local total=.5
        for i=1,n do total=total+forward(i,.25,.5) end
        return total
      end]],
    phases = {{48}, {64}},
  },
  {
    name = 'open-result-callm-loop',
    source = [[local function values(x) return x,.25,.5 end
      local function sum(a,b,c) return a+b+c end
      return function(n)
        local total=.5
        for i=1,n do total=total+sum(values(i)) end
        return total
      end]],
    phases = {{48}, {64}},
  },
  {
    name = 'scalar-iterator-root',
    -- The only loop in this runner is ITERC/ITERL. A helper FUNCF trace
    -- cannot count as a runner-root witness because root_headers binds
    -- trace IDs to the runner's own patched bytecode.
    source = [[local function iter(n,k) return k+1 end
      return function(n)
        local sum=.5
        for k in iter,n,0 do
          sum=sum+k
          if k>=n then break end
        end
        return sum
      end]],
    phases = {{64}, {'bad'}, {0}, {1}, {96}},
  },
  {
    name = 'nil-terminated-iterator-root',
    source = [[local function iter(n,k)
      if k<n then return k+1 end
      end
      return function(n)
        local sum=.5
        for k in iter,n,0 do sum=sum+k end
        return sum
      end]],
    phases = {{64}, {'bad'}, {0}, {1}, {96}},
  },
  {
    name = 'captured-stateful-multireturn-iterator-root',
    -- The iterator closes over mutable state and returns two values. This
    -- supplements the stateless one-result iterator cases above.
    source = [[local function make_iterator(step)
        local calls=0
        return function(n,k)
          calls=calls+1
          if k<n then return k+step,calls end
        end
      end
      local iter=make_iterator(1)
      return function(n)
        local sum=.5
        for k,ordinal in iter,n,0 do sum=sum+k+ordinal*.01 end
        return sum
      end]],
    phases = {{48}, {'bad'}, {16}, {64}},
  },
  {
    name = 'independent-two-root-sides',
    source = [[return function(x,y,n)
      local sum=.5
      for i=1,n do
        if x==1 then sum=sum+i*.25 end
        if y==1 then sum=sum-i*.5 end
      end
      return sum
    end]],
    phases = {{0,0,64}, {1,0,64}, {0,1,64}, {1,0,48}, {0,1,48}},
  },
  {
    name = 'independent-three-root-sides',
    source = [[return function(x,y,z,n)
      local sum=.5
      for i=1,n do
        if x==1 then sum=sum+i*.25 end
        if y==1 then sum=sum-i*.5 end
        if z==1 then sum=sum+i*.75 end
      end
      return sum
    end]],
    phases = {{0,0,0,64}, {1,0,0,64}, {0,1,0,64}, {0,0,1,64},
      {1,0,0,48}, {0,1,0,48}, {0,0,1,48}},
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
  {
    name = 'integer-for-sides',
    source = [[return function(mode,n)
      local sum=0
      for i=1,n do
        if mode==1 then sum=sum+i
        elseif mode==2 then sum=sum-i
        else sum=sum+i*3 end
      end
      return sum
    end]],
    phases = {{1,48}, {2,48}, {3,48}, {2,64}},
  },
  {
    name = 'integer-while-sides',
    source = [[return function(mode,n)
      local sum,i=0,1
      while i<=n do
        if mode==1 then sum=sum+i
        elseif mode==2 then sum=sum-i
        else sum=sum+i*3 end
        i=i+1
      end
      return sum
    end]],
    phases = {{1,48}, {2,48}, {3,48}, {2,64}},
  },
  {
    name = 'fractional-while-sides',
    -- Start with a nonintegral sum so a first INT-to-NUM transition does
    -- not consume the first side before the alternate branch is heated.
    source = [[return function(mode,n)
      local sum,i=.5,1
      while i<=n do
        if mode==1 then sum=sum+i*.25
        elseif mode==2 then sum=sum-i*.5
        else sum=sum+i*.75 end
        i=i+1
      end
      return sum
    end]],
    phases = {{1,48}, {2,48}, {3,48}, {2,64}},
  },
}

local function compile(source, oracle)
  local chunk=assert(loadstring(source, '@topology-probe'))
  -- Keep every helper prototype in the oracle tree interpreted too.
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
      if name == 'JFUNCF' or name == 'JFUNCV' or
         name == 'JFORL ' or name == 'JLOOP ' or
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

local function invoke_pair(candidate, oracle, args, oracle_args)
  local a,b
  oracle_args = oracle_args or args
  if args[#args] == 'bad' then
    a={pcall(candidate, unpack(args))}
    b={pcall(oracle, unpack(oracle_args))}
  else
    a={true,candidate(unpack(args))}
    b={true,oracle(unpack(oracle_args))}
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

local function peer_wait(r,s)
  assert(r:send(true,5)); assert(select(2,s:recv(120))); return true
end

local function main()
  local observed,active_candidate={},nil
  local ready,stop=threading.channel(1),threading.channel(1)
  local peer=assert(threading.spawn(peer_wait,ready,stop))
  local function exit_observer(no, exitno)
    -- Match at delivery; later collection may retire or reuse trace IDs.
    local roots=root_headers(active_candidate)
    local info=util.traceinfo(no)
    local kind,parent='other',nil
    if roots[no] then
      kind='root'
    elseif info and info.linktype == 'root' and roots[info.link] then
      kind,parent='root-side',info.link
    end
    observed[#observed+1]={no,exitno,kind,parent}
  end
  jit.off(exit_observer,true)
  assert(select(2,ready:recv(5)), 'MT peer did not start')
  io.write('case\tarch\tphase\troot_ids\tpublished_root_linked_sides\tobserved_root_exits\tobserved_root_side_exits\tobserved_other_exits\troot_side_exit_links\texit_counts\ttraces\n')
  io.flush()
  for _,case in ipairs(cases) do
    jit.flush()
    local candidate,candidate_helpers=compile(case.source, false)
    local oracle,oracle_helpers=compile(case.source, true)
    active_candidate=candidate
    jit.off(oracle,true)
    observed={}
    jit.opt.start('hotloop=1','hotexit=1','maxtrace=128')
    jit.attach(exit_observer,'texit')
    jit.on()
    local function case_args(args, helpers)
      if not case.helper_args then return args end
      local out={}
      for i=1,#helpers do out[#out+1]=helpers[i] end
      for i=1,#args do out[#out+1]=args[i] end
      return out
    end
    for phase,args in ipairs(case.phases) do
      for _=1,80 do
        invoke_pair(candidate,oracle,case_args(args,candidate_helpers),
          case_args(args,oracle_helpers))
      end
      collectgarbage('collect')
      invoke_pair(candidate,oracle,case_args(args,candidate_helpers),
        case_args(args,oracle_helpers))
      local roots=root_headers(candidate)
      local rows=trace_rows()
      local sides=side_ids(rows,roots)
      local root_exits,root_side_exits,other_exits=0,0,0
      local root_side_links={}
      local counts={}
      for _,event in ipairs(observed) do
        if event[3] == 'root' then
          root_exits=root_exits+1
        elseif event[3] == 'root-side' then
          root_side_exits=root_side_exits+1
          local key=event[1]..':'..event[4]
          root_side_links[key]=(root_side_links[key] or 0)+1
        else
          other_exits=other_exits+1
        end
        local key=event[1]..':'..event[2]
        counts[key]=(counts[key] or 0)+1
      end
      local events={}
      for key,count in pairs(counts) do events[#events+1]=key..'='..count end
      table.sort(events)
      local links={}
      for key,count in pairs(root_side_links) do
        links[#links+1]=key..'='..count
      end
      table.sort(links)
      if case.name == 'nonleaf-function-root-return' then
        local selected_root=false
        assert(next(roots), 'nonleaf FUNCF root was not published')
        for no in pairs(roots) do
          local info=rows[no]
          if info and info.linktype == 'return' then selected_root=true end
        end
        assert(selected_root, 'published nonleaf root has no return trace')
        if phase == 2 then
          assert(root_exits > 0,
            'nonleaf root did not enter natively and exit on bad argument')
        end
      elseif case.name == 'captured-two-result-call' then
        local selected_root=false
        assert(next(roots), 'captured pair-call FUNCF root was not published')
        for no in pairs(roots) do
          local info=rows[no]
          if info and info.linktype == 'return' then selected_root=true end
        end
        assert(selected_root, 'captured pair-call root has no return trace')
        if phase == 2 then
          assert(root_exits > 0,
            'captured pair-call root did not enter natively and exit on bad argument')
        end
      elseif case.name == 'function-two-results' then
        local selected_root=false
        assert(next(roots), 'two-result FUNCF root was not published')
        for no in pairs(roots) do
          local info=rows[no]
          if info and info.linktype == 'return' then selected_root=true end
        end
        assert(selected_root, 'two-result FUNCF root has no return trace')
        if phase == 2 then
          assert(root_exits > 0,
            'two-result root did not enter natively and exit on bad argument')
        end
      end
      io.write(string.format('%s\t%s\t%d\t%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\n',
        case.name,jit.arch,phase,id_list(roots),id_list(sides),
        root_exits,root_side_exits,other_exits,table.concat(links,','),
        table.concat(events,','),describe(rows)))
      io.flush()
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
jit.off(id_list,true)
jit.off(side_ids,true)
jit.off(equal,true)
jit.off(invoke_pair,true)
jit.off(peer_wait,true)
jit.off(main,true)
main()
