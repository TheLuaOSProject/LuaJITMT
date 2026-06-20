local th = require"threading"

local worker = th.spawn(function()
  local trace_count = require"jit_harness".trace_count

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")

  local function branch(n, flag)
    local s = 0
    for i = 1, n do
      if flag and i == 10 then
	s = s + 1000
      else
	s = s + i
      end
    end
    return s
  end

  for _ = 1, 20 do
    assert(branch(80, false) == 3240)
  end
  local root_traces = trace_count(32)
  assert(root_traces > 0)

  for _ = 1, 20 do
    assert(branch(80, true) == 4230)
  end
  local side_traces = trace_count(32)
  assert(side_traces > root_traces)

  return root_traces, side_traces, th.current():id()
end)

local ok, root_traces, side_traces, tid = worker:join()
assert(ok == true)
assert(type(root_traces) == "number" and root_traces > 0)
assert(type(side_traces) == "number" and side_traces > root_traces)
assert(tid == worker:id())

print("t-jit-secondary OK: secondary TG records, enters, and side-traces x64 mcode")
