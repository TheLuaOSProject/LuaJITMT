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
end
jit.off(run_case,true); jit.off(main,true)
main()
assert(stop:send(true,5))
local joined,result=peer:join(5)
assert(joined and result)
io.write("open CALLM consumer-exit interpreter companion OK\n")
