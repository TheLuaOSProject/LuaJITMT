-- Ordinary Lua publication/oracle companion for bounded fixed CALL/VARG leaves.
-- A published runner loop is not evidence that ARM64 executed native code.

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
end
jit.off(loop_trace, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(120)))
  return true
end, ready, stop))
assert(select(2, ready:recv(5)), "MT peer did not become ready")

local cases = {
  {"two-requested-two-supplied", "local x,y=...; return x+y",
   "local sum=0; for i=1,n do sum=sum+fn(i,.25) end; return sum", {17}},
  {"pad-unused-varargs", "local x,y,z=...; return x+1",
   "local sum=0; for i=1,n do sum=sum+fn(i) end; return sum", {17}},
  {"truncate-unused-extras", "local x=...; return x+1",
   "local sum=0; for i=1,n do sum=sum+fn(i,20,30) end; return sum", {17}},
  {"primitive-result", "local x=...; return true",
   "local sum=0; for i=1,n do if fn(i,.25) then sum=sum+1 end end; return sum", {17}},
}

local function compile(label, body, runner)
  local source = "local leaf=function(...) " .. body .. " end\n" ..
    "local run=function(fn,n) " .. runner .. " end\nreturn leaf,run"
  local chunk, err = loadstring(source, "@lua-vararg-" .. label)
  assert(chunk, err)
  return chunk()
end
jit.off(compile, true)

local function run_case(case)
  local label, body, runner, sizes = unpack(case)
  local leaf, run = compile(label, body, runner)
  local oracle_leaf, oracle_run = compile("oracle-" .. label, body, runner)
  jit.off(oracle_leaf, true); jit.off(oracle_run, true)
  local trace, attempts
  for attempt = 1, 8 do
    attempts = attempt
    local expected = oracle_run(oracle_leaf, sizes[1])
    jit.flush(); jit.on()
    jit.opt.start("hotloop=1", "hotexit=1000", "maxtrace=64")
    local actual = run(leaf, sizes[1])
    assert(actual == expected, label .. " differs from interpreter oracle")
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, label .. " did not publish the admitted runner trace")
  local reuse_n = 9
  local expected = oracle_run(oracle_leaf, reuse_n)
  assert(run(leaf, reuse_n) == expected, label .. " changed on trace reuse")
  assert(loop_trace(run) == trace, label .. " did not reuse the same trace")
  io.write(string.format(
    "lua-varargs case=%s expected=publish attempts=%d trace=%d reuse_same=yes oracle_match=yes\n",
    label, attempts, trace))
end
jit.off(run_case, true)

local function run_boundary(label, leaf_source, runner_source)
  local chunk, err = loadstring("local leaf=function(...) " .. leaf_source .. " end\n" ..
    "local run=function(fn,n) " .. runner_source .. " end\nreturn leaf,run",
    "@lua-vararg-refusal-" .. label)
  assert(chunk, err)
  local leaf, run = chunk()
  local oracle_chunk = assert(loadstring("local leaf=function(...) " .. leaf_source .. " end\n" ..
    "local run=function(fn,n) " .. runner_source .. " end\nreturn leaf,run",
    "@lua-vararg-oracle-" .. label))
  local oracle_leaf, oracle_run = oracle_chunk()
  jit.off(oracle_leaf, true); jit.off(oracle_run, true)
  local expected = oracle_run(oracle_leaf, 20)
  jit.flush(); jit.on(); jit.opt.start("hotloop=1", "hotexit=1000", "maxtrace=64")
  assert(run(leaf, 20) == expected, label .. " differs from interpreter oracle")
  local trace = loop_trace(run)
  io.write("lua-varargs case=" .. label ..
    " generic-runtime-trace=" .. (trace and tostring(trace) or "-") ..
    " oracle_match=yes (architecture admission is checked by the C fixture)\n")
end
jit.off(run_boundary, true)

local function run_sret_case()
  local path = os.getenv("LJ_ARM64_FFI_VARARGS_LIB")
  if not path or path == "" then return end
  local ffi = require("ffi")
  ffi.cdef[[
    struct arm64_va_sret24 { uint64_t lo, mid, hi; };
    struct arm64_va_sret24 arm64_va_sret_cdata_tail(uint32_t, ...);
  ]]
  local lib = ffi.load(path)
  local fn = lib.arm64_va_sret_cdata_tail
  local a, b = ffi.new("int32_t", -17), ffi.new("double", 2.5)
  local function make_runner()
    return assert(loadstring([[
      return function(fn,a,b,n)
        local out; local sum=0.5
        for i=1,n do out=fn(23,a,b); sum=sum+i end
        return out.lo==0x112233445566 and out.mid==0x123456789abc and
          out.hi==0x6543210 and sum==n*(n+1)/2+0.5
      end
    ]], "@lua-vararg-indirect-result"))()
  end
  local run, oracle = make_runner(), make_runner()
  jit.off(oracle, true)
  local trace, attempts
  for attempt = 1, 8 do
    attempts = attempt
    local expected = oracle(fn, a, b, 24)
    jit.flush(); jit.on()
    jit.opt.start("hotloop=1", "hotexit=1000", "maxtrace=64")
    assert(run(fn, a, b, 24) == expected,
      "variadic indirect-result CALLXS differs from interpreter oracle")
    trace = loop_trace(run)
    if trace then break end
  end
  assert(trace, "variadic indirect-result runner did not publish a loop trace")
  collectgarbage("collect")
  assert(run(fn, a, b, 9) == oracle(fn, a, b, 9),
    "variadic indirect-result result changed after GC/reuse")
  assert(loop_trace(run) == trace,
    "variadic indirect-result runner did not reuse the same trace")
  io.write(string.format(
    "lua-varargs case=variadic-indirect-result attempts=%d trace=%d oracle_match=yes reuse_same=yes native_claim=no\n",
    attempts, trace))
end
jit.off(run_sret_case, true)

local function main()
  for _, case in ipairs(cases) do run_case(case) end
  run_boundary("open-varg", "return ...",
    "local sum=0; for i=1,n do local a,b=fn(i,.25); sum=sum+a+b end; return sum")
  run_sret_case()
end
jit.off(main, true)
main()

assert(stop:send(true, 5), "MT peer stop send failed")
local joined, result = peer:join(5)
assert(joined and result, "MT peer did not join cleanly")
io.write("Lua varargs ordinary OK: fixed CALL/VARG oracle/publication plus open-result semantic baseline; no native-execution claim\n")
