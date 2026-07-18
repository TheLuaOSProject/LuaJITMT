local ffi = require("ffi")
local threading = require("threading")

if jit.arch ~= "x64" then
  print("t-ffi-callxs-remote-flush SKIP: x64-only lowering")
  return
end

ffi.cdef [[
void lj_callxs_flush_configure(int32_t, int32_t);
void lj_callxs_flush_unblock(void);
int32_t lj_callxs_flush_entered_count(void);
int32_t lj_callxs_flush_entry_count(void);
int32_t lj_callxs_flush_effect_count(void);
int32_t lj_callxs_flush_maybe_block(int32_t);
]]

local so = assert(os.getenv("LJ_M7_FFI_CALLXS_FLUSH_SO"))
local lib = ffi.load(so)
local ready = threading.channel(1)
local start = threading.channel(1)

local worker = threading.spawn(function(ready_ch, start_ch, so_path)
  local bit = require("bit")
  local ffi = require("ffi")
  local util = require("jit.util")
  local vmdef = require("jit.vmdef")

  ffi.cdef [[
  void lj_callxs_flush_configure(int32_t, int32_t);
  int32_t lj_callxs_flush_maybe_block(int32_t);
  ]]
  local worker_lib = ffi.load(so_path)

  local function trace_op_count(wanted)
    local count = 0
    for tr = 1, 256 do
      local info = util.traceinfo(tr)
      if info then
        for ref = 1, info.nins do
          local _, ot = util.traceir(tr, ref)
          if ot then
            local opidx = bit.rshift(ot, 8)
            local op = vmdef.irnames:sub(opidx * 6 + 1, opidx * 6 + 6)
            if op == wanted then count = count + 1 end
          end
        end
      end
    end
    return count
  end
  jit.off(trace_op_count, true)

  local function run(base, n)
    local sum = 0
    for i = 1, n do
      sum = sum + worker_lib.lj_callxs_flush_maybe_block(base + i)
    end
    return sum
  end

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  worker_lib.lj_callxs_flush_configure(-2147483647 - 1, 0)
  for i = 1, 80 do
    local base = i * 16
    assert(run(base, 8) == 8 * base + 36 + 8 * 9)
  end
  local xsave = trace_op_count("XSAVE ")
  local callxs = trace_op_count("CALLXS")
  assert(xsave > 0, "blocking target did not record XSAVE")
  assert(callxs > 0, "blocking target did not record CALLXS")
  assert(ready_ch:send({ xsave = xsave, callxs = callxs }, 10) == true)

  local command, ok = start_ch:recv(10)
  assert(ok == true and command == "block")
  return run(100, 8)
end, ready, start, so)

local recorded, ready_ok = ready:recv(10)
assert(ready_ok == true and type(recorded) == "table")
assert(recorded.xsave > 0 and recorded.callxs > 0)

-- The first loop iteration reaches the patched loop backedge. The gate is on
-- the eighth iteration, which therefore executes from the already-recorded
-- generic CALLXS trace rather than from the interpreter warm-up edge.
lib.lj_callxs_flush_configure(108, 1)
assert(start:send("block", 10) == true)

local deadline = threading.now() + 10
while lib.lj_callxs_flush_entered_count() == 0 and
      threading.now() < deadline do
  threading.sleep(0.001)
end
assert(lib.lj_callxs_flush_entered_count() == 1,
       "generated CALLXS did not enter the blocker")
assert(lib.lj_callxs_flush_entry_count() == 1,
       "blocking call was entered more than once")
assert(lib.lj_callxs_flush_effect_count() == 0,
       "blocking call completed before the flush")

-- This ordering is the regression oracle: the target cannot leave C until the
-- line after jit.flush(), so a return proves the remote trace-flush handshake
-- admitted its stable ACTIVE native-frame certificate.
local before = threading.now()
jit.flush()
local elapsed = threading.now() - before
assert(elapsed < 2,
       ("remote flush stalled for %.3fs on certified CALLXS"):format(elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0,
       "worker escaped the blocker during remote flush")

lib.lj_callxs_flush_unblock()
local joined, result = worker:join(10)
assert(joined == true, tostring(result))
assert(result == 908, "retired CALLXS trace returned the wrong result")
assert(lib.lj_callxs_flush_entry_count() == 1,
       "blocking call was re-entered after trace retirement")
assert(lib.lj_callxs_flush_effect_count() == 1,
       "blocking call side effect did not complete exactly once")

print(("t-ffi-callxs-remote-flush OK: flush returned in %.3fs before release"):
      format(elapsed))
