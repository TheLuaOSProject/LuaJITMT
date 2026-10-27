-- Standalone ordinary-runtime publication/refusal probe for pure Lua leaves.
-- Publication means the runner's JFORL points at a live self-linked loop
-- trace. It does not claim that the trace executed natively.

local threading = require("threading")
local util, vmdef, bit = require("jit.util"), require("jit.vmdef"), require("bit")
assert(jit.status(), "JIT must be enabled")
jit.off(true, true)

local function loop_trace(fn)
  local nbc = util.funcinfo(fn).bytecodes
  for pc = 1, nbc-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      if vmdef.bcnames:sub(op*6+1, op*6+6) == "JFORL " then
        local id = bit.rshift(ins, 16)
        local info = util.traceinfo(id)
        if info and info.linktype == "loop" and info.link == id then
          return id
        end
      end
    end
  end
  return nil
end
jit.off(loop_trace, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(60)))
  return true
end, ready, stop))
assert(select(2, ready:recv(5)), "MT peer did not become ready")

local function compile_pair(label, leaf_body, run_body, leaf_prefix)
  local src = (leaf_prefix or "") ..
    "local leaf=function(x) " .. leaf_body .. " end\n" ..
    "local run=function(fn,n) " .. run_body .. " end\n" ..
    "return leaf,run"
  local chunk, err = loadstring(src, "@lua-leaf-" .. label)
  assert(chunk, err)
  return chunk()
end

local simple_run = "local sum=.5; for i=1,n do sum=sum+fn(i+.5) end; return sum"

local positives = {
  {"addvn", "return x+0.25"},
  {"subvn", "return x-0.25"},
  {"mulvn", "return x*0.5"},
  {"divvn", "return x/2"},
  {"addnv", "return 0.25+x"},
  {"subnv", "return 0.25-x"},
  {"mulnv", "return 0.5*x"},
  {"divnv", "return 0.5/x"},
  {"addvv-local-number", "local bias=0.25; return x+bias"},
}

local negatives = {
  {"upvalue", "return x+bias", simple_run, "local bias=0.25;\n"},
  {"branch", "if x>0 then return x+0.25 else return x-0.25 end", simple_run},
  {"nested-call", "return inner(x)", simple_run,
    "local inner=function(y) return y+0.25 end\n"},
  {"vararg-callee", "return ...", simple_run, nil, true},
  {"extra-call-argument", "return x+0.25",
    "local sum=.5; for i=1,n do sum=sum+fn(i+.5,0.25) end; return sum"},
  {"multiple-results", "return x,x+0.5",
    "local sum=.5; for i=1,n do local a,b=fn(i+.5); sum=sum+a+b end; return sum"},
  {"child-argument-write", "x=x+3; return x*2",
    "local sum=0; for i=1,n do sum=sum+fn(i) end; return sum"},
}

local function run_positive(label, body)
  local leaf, run = compile_pair(label, body, simple_run)
  local oracle_leaf, oracle_run = compile_pair("oracle-"..label, body, simple_run)
  assert(leaf ~= oracle_leaf and run ~= oracle_run)
  jit.off(oracle_leaf, true); jit.off(oracle_run, true)
  local n=48
  local expected=oracle_run(oracle_leaf,n)

  jit.flush()
  jit.on()
  jit.opt.start("hotloop=1","hotexit=1000","maxtrace=64")
  local trace, result, attempts
  for attempt=1,8 do
    attempts=attempt
    result=run(leaf,n)
    assert(result==expected, label.." candidate/oracle result mismatch")
    trace=loop_trace(run)
    if trace then break end
  end
  assert(trace, label.." did not publish within 8 cold attempts")

  local reuse_n=24
  local reuse_expected=oracle_run(oracle_leaf,reuse_n)
  local again=run(leaf,reuse_n)
  local again_trace=loop_trace(run)
  assert(again==reuse_expected, label.." result changed on reuse")
  assert(again_trace==trace, label.." did not reuse the exact loop trace")
  local reused="yes"
  io.write(string.format(
    "lua-leaf case=%s expected=publish attempts=%d trace=%s reuse_same=%s result_match=yes\n",
    label,attempts,trace and tostring(trace) or "-",reused))
end
jit.off(run_positive, true)

local function run_negative(case)
  local label, body, runner_body, prefix, use_varargs = unpack(case,1,5)
  runner_body=runner_body or simple_run
  local leaf_src
  if use_varargs then
    leaf_src="local leaf=function(...) "..body.." end\n"
  else
    leaf_src="local leaf=function(x) "..body.." end\n"
  end
  local src=(prefix or "")..leaf_src..
    "local run=function(fn,n) "..runner_body.." end\nreturn leaf,run"
  local chunk,err=loadstring(src,"@lua-leaf-negative-"..label)
  assert(chunk,err)
  local leaf,run=chunk()
  local ochunk=assert(loadstring(src,"@lua-leaf-oracle-"..label))
  local oracle_leaf,oracle_run=ochunk()
  jit.off(oracle_leaf,true); jit.off(oracle_run,true)
  local n=48
  local oracle_ok,expected=pcall(oracle_run,oracle_leaf,n)
  assert(oracle_ok, label.." oracle failed: "..tostring(expected))
  jit.flush()
  jit.on()
  jit.opt.start("hotloop=1","hotexit=1000","maxtrace=64")
  local trace,result,attempts,failure
  for attempt=1,8 do
    attempts=attempt
    local ok,value=pcall(run,leaf,n)
    if not ok then failure=value; break end
    result=value
    assert(result==expected, label.." candidate/oracle result mismatch")
    trace=loop_trace(run)
    if trace then break end
  end
  assert(not failure, label.." candidate failed: "..tostring(failure))
  assert(not trace, label.." unexpectedly published a loop trace")
  local same=true
  local reused="-"
  io.write(string.format(
    "lua-leaf case=%s expected=refuse attempts=%d trace=%s reuse_same=%s result_match=%s error=%s\n",
    label,attempts,trace and tostring(trace) or "-",reused,
    same and "yes" or "no",failure and tostring(failure) or "-"))
end
jit.off(run_negative, true)

local function main()
  for _,c in ipairs(positives) do run_positive(c[1],c[2]) end
  for _,c in ipairs(negatives) do run_negative(c) end
end
jit.off(compile_pair, true)
jit.off(main, true)
main()

assert(stop:send(true,5), "peer stop send failed")
local joined, result=peer:join(5)
assert(joined and result, "MT peer did not join cleanly")
io.write(string.format('Lua leaf ordinary OK: publications=%d exact-trace-reuse=%d refusal-shapes=%d (8 calls each); no native-execution claim\n', #positives, #positives, #negatives))
