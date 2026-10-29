-- Interpreter oracle for consumer-identity changes after open producer calls.
local th=require('threading')
local ready,stop=th.channel(1),th.channel(1)
local peer=assert(th.spawn(function(a,b)
  assert(a:send(true,5))
  assert(select(2,b:recv(60)))
  return true
end,ready,stop))
assert(select(2,ready:recv(5)))

local cases={
  {"return x", "return a+1", "return a+17", "a"},
  {"return x,.25", "return a+b+2", "return a+101*b+17", "a,b"},
  {"return x,.25,.5", "return a+b+c+3",
   "return a+101*b+10007*c+17", "a,b,c"},
}

local function run_case(n,case)
  local make_consumer=function(body)
    return assert(loadstring("return function("..case[4]..") "..body.." end",
                            "@open-callm-interpreter-consumer"))()
  end
  local consumer_a,consumer_b=make_consumer(case[2]),make_consumer(case[3])
  local function make_runner()
    return function(fn,consumer,count)
      local total=.5
      for i=1,count do total=total+consumer(fn(i+.5)) end
      return total
    end
  end
  local candidate=make_runner()
  local oracle=make_runner()
  local producer=assert(loadstring("return function(x) "..case[1].." end",
                                  "@open-callm-interpreter-producer"))()
  jit.off(candidate,true); jit.off(oracle,true); jit.off(producer,true)
  for _,consumer in ipairs({consumer_a,consumer_b}) do
    assert(candidate(producer,consumer,n)==oracle(producer,consumer,n))
  end
  io.write(string.format("open CALLM interpreter results=%d consumer-swap oracle matched\n",n))
end

local function main()
  for n=1,3 do run_case(n,cases[n]) end
  for n,case in ipairs(cases) do
    local function consumer(body)
      return assert(loadstring("return function("..case[4]..") "..body.." end", "@captured-open-callm-consumer"))()
    end
    local a,b=consumer(case[2]),consumer(case[3])
    local function make_runner(initial)
      local current=initial
      local function run(fn,count)
        local total=.5
        for i=1,count do total=total+current(fn(i+.5)) end
        return total
      end
      local function set(value) current=value end
      return run,set
    end
    local candidate,set_candidate=make_runner(a)
    local oracle,set_oracle=make_runner(a)
    local producer=assert(loadstring("return function(x) "..case[1].." end",
                                    "@captured-open-callm-producer"))()
    jit.off(a,true); jit.off(b,true)
    jit.off(candidate,true); jit.off(set_candidate,true)
    jit.off(oracle,true); jit.off(set_oracle,true); jit.off(producer,true)
    assert(candidate(producer,64)==oracle(producer,64))
    set_candidate(b); set_oracle(b)
    assert(candidate(producer,64)==oracle(producer,64))
    set_candidate(a); set_oracle(a)
    assert(candidate(producer,64)==oracle(producer,64))
    io.write(string.format("captured open CALLM interpreter results=%d consumer swaps matched\n",n))
  end
  for n,results in ipairs({"1.25", "1.25,.25", "1.25,.25,.5"}) do
    local function make_producer()
      return assert(loadstring("return function() return "..results.." end",
                               "@zero-formal-open-callm-producer"))()
    end
    local function make_consumer(bias)
      return assert(loadstring("return function(a,b,c) return (a or 0)+(b or 0)+(c or 0)+"..
                               bias.." end",
                               "@zero-formal-open-callm-consumer"))()
    end
    local ca,cb=make_consumer("1"),make_consumer("17")
    local oa,ob=make_consumer("1"),make_consumer("17")
    local function make_runner(initial)
      local current=initial
      local function run(fn,count)
        local total=.5
        for i=1,count do total=total+current(fn()) end
        return total
      end
      local function set(value) current=value end
      return run,set
    end
    local candidate,set_candidate=make_runner(ca)
    local oracle,set_oracle=make_runner(oa)
    local p,po=make_producer(),make_producer()
    for _,fn in ipairs({candidate,set_candidate,oracle,set_oracle,p,po,ca,cb,oa,ob}) do
      jit.off(fn,true)
    end
    assert(candidate(p,96)==oracle(po,96))
    set_candidate(cb); set_oracle(ob)
    assert(candidate(p,64)==oracle(po,64))
    set_candidate(ca); set_oracle(oa)
    assert(candidate(p,64)==oracle(po,64))
    io.write(string.format("zero-formal captured producer results=%d interpreter swaps matched\n",n))
  end
end
jit.off(run_case,true); jit.off(main,true)
main()
assert(stop:send(true,5))
local joined,result=peer:join(5)
assert(joined and result)
io.write("open CALLM consumer-exit interpreter companion OK\n")
