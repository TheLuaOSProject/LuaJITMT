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
int32_t lj_callxs_flush_aggregate_call_count(void);
int32_t lj_callxs_flush_maybe_block(int32_t);
int32_t *lj_callxs_flush_ptr_maybe_block(int32_t *, int32_t);
_Bool lj_callxs_flush_bool_maybe_block(int32_t, int32_t);
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
  int32_t lj_callxs_flush_aggregate_call_count(void);
  int32_t lj_callxs_flush_maybe_block(int32_t);
  int32_t *lj_callxs_flush_ptr_maybe_block(int32_t *, int32_t);
  _Bool lj_callxs_flush_bool_maybe_block(int32_t, int32_t);
  struct lj_callxs_flush_aggregate {
    uint32_t magic_hi;
    uint32_t magic_lo;
    double weight;
    int32_t code;
    uint32_t stamp;
  };
  struct lj_callxs_flush_aggregate
    lj_callxs_flush_aggregate_maybe_block(double, int32_t);
  ]]
  local worker_lib = ffi.load(so_path)
  assert(ffi.sizeof("struct lj_callxs_flush_aggregate") == 24)

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

  local function run_scalar(base, n)
    local sum = 0
    for i = 1, n do
      sum = sum + worker_lib.lj_callxs_flush_maybe_block(base + i)
    end
    return sum
  end

  local function run_pointer(ptr, base, n)
    local result
    for i = 1, n do
      result = worker_lib.lj_callxs_flush_ptr_maybe_block(ptr, base + i)
    end
    return result
  end

  local function run_bool(base, n, truth)
    local result
    for i = 1, n do
      result = worker_lib.lj_callxs_flush_bool_maybe_block(base + i, truth)
    end
    return result
  end

  local function run_aggregate(base, n)
    local result
    for i = 1, n do
      result = worker_lib.lj_callxs_flush_aggregate_maybe_block(
        0.5, base + i)
    end
    return result
  end

  local function assert_aggregate(result, value)
    assert(ffi.istype("struct lj_callxs_flush_aggregate", result))
    assert(result.magic_hi == 0xfedc0000 + value)
    assert(result.magic_lo == 0x76540000 + value)
    assert(result.weight == 0.5 + value * 0.25)
    assert(result.code == value * 3 - 17)
    assert(result.stamp == 0x13570000 + value)
  end
  jit.off(assert_aggregate, true)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  worker_lib.lj_callxs_flush_configure(-2147483647 - 1, 0)
  for i = 1, 80 do
    local base = i * 16
    assert(run_scalar(base, 8) == 8 * base + 36 + 8 * 9)
  end
  local scalar_xsave = trace_op_count("XSAVE ")
  local scalar_callxs = trace_op_count("CALLXS")
  assert(scalar_xsave > 0, "scalar blocker did not record XSAVE")
  assert(scalar_callxs > 0, "scalar blocker did not record CALLXS")

  jit.flush()
  local ptr = ffi.new("int32_t[1]", 1234)
  for i = 1, 80 do
    local base = i * 16
    local result = run_pointer(ptr, base, 8)
    assert(result == ptr and ffi.istype("int32_t *", result))
  end
  local boxed_xsave = trace_op_count("XSAVE ")
  local boxed_callxs = trace_op_count("CALLXS")
  assert(boxed_xsave > 0, "boxed blocker did not record XSAVE")
  assert(boxed_callxs > 0, "boxed blocker did not record CALLXS")
  assert(ready_ch:send({
    phase = "pointer_ready",
    scalar_xsave = scalar_xsave,
    scalar_callxs = scalar_callxs,
    boxed_xsave = boxed_xsave,
    boxed_callxs = boxed_callxs,
  }, 10) == true)

  local command, ok = start_ch:recv(10)
  assert(ok == true and command == "block_pointer")
  local pointer_result = run_pointer(ptr, 100, 8)
  assert(pointer_result == ptr and ffi.istype("int32_t *", pointer_result))
  assert(ready_ch:send({
    phase = "pointer_done",
    result = tonumber(pointer_result[0]),
  }, 10) == true)
  command, ok = start_ch:recv(10)
  assert(ok == true and command == "continue")

  -- Record the boolean specialization as true after the pointer trace has
  -- been retired, then block while returning false from the same trace.
  jit.flush()
  worker_lib.lj_callxs_flush_configure(-2147483647 - 1, 0)
  for i = 1, 80 do
    assert(run_bool(i * 16, 8, 1) == true)
  end
  local bool_xsave = trace_op_count("XSAVE ")
  local bool_callxs = trace_op_count("CALLXS")
  assert(bool_xsave > 0, "boolean blocker did not record XSAVE")
  assert(bool_callxs > 0, "boolean blocker did not record CALLXS")
  assert(ready_ch:send({
    phase = "bool_ready",
    bool_xsave = bool_xsave,
    bool_callxs = bool_callxs,
  }, 10) == true)
  command, ok = start_ch:recv(10)
  assert(ok == true and command == "block_bool")
  local bool_result = run_bool(100, 8, 0)
  assert(type(bool_result) == "boolean" and bool_result == false)
  assert(ready_ch:send({
    phase = "bool_done",
    boolean = bool_result,
  }, 10) == true)
  command, ok = start_ch:recv(10)
  assert(ok == true and command == "continue_aggregate")

  -- The result cdata is created before native entry and is reachable only
  -- through the ACTIVE native-frame root while the sret callee is blocked.
  -- Warm the exact indirect-result call independently of the boxed paths.
  jit.flush()
  worker_lib.lj_callxs_flush_configure(-2147483647 - 1, 0)
  for i = 1, 80 do
    local base = i * 16
    assert_aggregate(run_aggregate(base, 8), base + 8)
  end
  assert(worker_lib.lj_callxs_flush_aggregate_call_count() == 80 * 8)
  local aggregate_xsave = trace_op_count("XSAVE ")
  local aggregate_callxs = trace_op_count("CALLXS")
  assert(aggregate_xsave > 0, "sret blocker did not record XSAVE")
  assert(aggregate_callxs > 0, "sret blocker did not record CALLXS")
  assert(ready_ch:send({
    phase = "aggregate_ready",
    aggregate_xsave = aggregate_xsave,
    aggregate_callxs = aggregate_callxs,
    warm_calls = worker_lib.lj_callxs_flush_aggregate_call_count(),
  }, 10) == true)
  command, ok = start_ch:recv(10)
  assert(ok == true and command == "block_aggregate")
  local aggregate_result = run_aggregate(100, 8)
  assert_aggregate(aggregate_result, 108)
  return {
    pointer = tonumber(pointer_result[0]),
    boolean = bool_result,
    aggregate = {
      magic_hi = aggregate_result.magic_hi,
      magic_lo = aggregate_result.magic_lo,
      weight = aggregate_result.weight,
      code = aggregate_result.code,
      stamp = aggregate_result.stamp,
    },
  }
end, ready, start, so)

local recorded, ready_ok = ready:recv(10)
if ready_ok ~= true or type(recorded) ~= "table" then local done, why = worker:join(0); error("ready first="..tostring(recorded).." second="..tostring(ready_ok).."; joined="..tostring(done).." result="..tostring(why)) end
assert(ready_ok == true and type(recorded) == "table")
assert(recorded.phase == "pointer_ready")
assert(recorded.scalar_xsave > 0 and recorded.scalar_callxs > 0)
assert(recorded.boxed_xsave > 0 and recorded.boxed_callxs > 0)

-- The first loop iteration reaches the patched loop backedge. The gate is on
-- the eighth iteration, which therefore executes from the already-recorded
-- generic CALLXS trace rather than from the interpreter warm-up edge.
lib.lj_callxs_flush_configure(108, 1)
assert(start:send("block_pointer", 10) == true)

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

-- A full GC while the box is absent from the worker's Lua stack must retain it
-- through the exact ACTIVE native-frame root. The target cannot leave C before
-- the explicit unblock, so both GC and flush prove their handshakes admit that
-- stable frame without waiting for the foreign call.
local gc_before = threading.now()
collectgarbage("collect")
local pointer_gc_elapsed = threading.now() - gc_before
assert(pointer_gc_elapsed < 2,
       ("remote GC stalled for %.3fs on rooted boxed CALLXS"):format(
         pointer_gc_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0,
       "worker escaped the blocker during remote GC")

local before = threading.now()
jit.flush()
local pointer_flush_elapsed = threading.now() - before
assert(pointer_flush_elapsed < 2,
       ("remote flush stalled for %.3fs on certified CALLXS"):format(
         pointer_flush_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0,
       "worker escaped the blocker during remote flush")

lib.lj_callxs_flush_unblock()
local pointer_done, pointer_done_ok = ready:recv(10)
assert(pointer_done_ok == true and pointer_done.phase == "pointer_done")
assert(pointer_done.result == 1234,
       "retired boxed CALLXS returned the wrong pointer")
assert(lib.lj_callxs_flush_entry_count() == 1,
       "blocking call was re-entered after trace retirement")
assert(lib.lj_callxs_flush_effect_count() == 1,
       "blocking call side effect did not complete exactly once")
assert(start:send("continue", 10) == true)

local bool_ready, bool_ready_ok = ready:recv(10)
assert(bool_ready_ok == true and bool_ready.phase == "bool_ready")
assert(bool_ready.bool_xsave > 0 and bool_ready.bool_callxs > 0)
lib.lj_callxs_flush_configure(108, 1)
assert(start:send("block_bool", 10) == true)

deadline = threading.now() + 10
while lib.lj_callxs_flush_entered_count() == 0 and
      threading.now() < deadline do
  threading.sleep(0.001)
end
assert(lib.lj_callxs_flush_entered_count() == 1,
       "generated boolean CALLXS did not enter the blocker")
assert(lib.lj_callxs_flush_entry_count() == 1)
assert(lib.lj_callxs_flush_effect_count() == 0)

gc_before = threading.now()
collectgarbage("collect")
local bool_gc_elapsed = threading.now() - gc_before
assert(bool_gc_elapsed < 2,
       ("remote GC stalled for %.3fs on boolean CALLXS"):format(
         bool_gc_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0)

before = threading.now()
jit.flush()
local bool_flush_elapsed = threading.now() - before
assert(bool_flush_elapsed < 2,
       ("remote flush stalled for %.3fs on boolean CALLXS"):format(
         bool_flush_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0)

lib.lj_callxs_flush_unblock()
local bool_done, bool_done_ok = ready:recv(10)
assert(bool_done_ok == true and bool_done.phase == "bool_done")
assert(type(bool_done.boolean) == "boolean" and bool_done.boolean == false,
       "retired boolean CALLXS restored a non-boolean or wrong value")
assert(lib.lj_callxs_flush_entry_count() == 1,
       "boolean blocker was re-entered after trace retirement")
assert(lib.lj_callxs_flush_effect_count() == 1,
       "boolean blocker side effect did not complete exactly once")
assert(start:send("continue_aggregate", 10) == true)

local aggregate_ready, aggregate_ready_ok = ready:recv(10)
assert(aggregate_ready_ok == true and
       aggregate_ready.phase == "aggregate_ready")
assert(aggregate_ready.aggregate_xsave > 0 and
       aggregate_ready.aggregate_callxs > 0)
assert(aggregate_ready.warm_calls == 80 * 8,
       "sret warm-up call count was not exact")

lib.lj_callxs_flush_configure(108, 1)
assert(start:send("block_aggregate", 10) == true)

deadline = threading.now() + 10
while lib.lj_callxs_flush_entered_count() == 0 and
      threading.now() < deadline do
  threading.sleep(0.001)
end
assert(lib.lj_callxs_flush_entered_count() == 1,
       "generated sret CALLXS did not enter the blocker")
assert(lib.lj_callxs_flush_entry_count() == 1,
       "blocking sret call was entered more than once")
assert(lib.lj_callxs_flush_effect_count() == 0,
       "blocking sret call completed before remote pressure")
assert(lib.lj_callxs_flush_aggregate_call_count() == 8,
       "sret trace did not reach the blocking eighth call exactly once")

gc_before = threading.now()
collectgarbage("collect")
local aggregate_gc_elapsed = threading.now() - gc_before
assert(aggregate_gc_elapsed < 2,
       ("remote GC stalled for %.3fs on rooted sret CALLXS"):format(
         aggregate_gc_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0,
       "sret callee escaped before remote GC completed")
assert(lib.lj_callxs_flush_aggregate_call_count() == 8,
       "remote GC replayed the blocked sret call")

before = threading.now()
jit.flush()
local aggregate_flush_elapsed = threading.now() - before
assert(aggregate_flush_elapsed < 2,
       ("remote flush stalled for %.3fs on rooted sret CALLXS"):format(
         aggregate_flush_elapsed))
assert(lib.lj_callxs_flush_effect_count() == 0,
       "sret callee escaped before remote flush completed")
assert(lib.lj_callxs_flush_aggregate_call_count() == 8,
       "remote flush replayed the blocked sret call")

lib.lj_callxs_flush_unblock()
local joined, result = worker:join(10)
assert(joined == true, tostring(result))
assert(type(result) == "table" and result.pointer == 1234)
assert(type(result.boolean) == "boolean" and result.boolean == false)
assert(type(result.aggregate) == "table")
assert(result.aggregate.magic_hi == 0xfedc0000 + 108)
assert(result.aggregate.magic_lo == 0x76540000 + 108)
assert(result.aggregate.weight == 0.5 + 108 * 0.25)
assert(result.aggregate.code == 108 * 3 - 17)
assert(result.aggregate.stamp == 0x13570000 + 108)
assert(lib.lj_callxs_flush_entry_count() == 1,
       "sret blocker was re-entered after trace retirement")
assert(lib.lj_callxs_flush_effect_count() == 1,
       "sret blocker side effect did not complete exactly once")
assert(lib.lj_callxs_flush_aggregate_call_count() == 8,
       "retired sret CALLXS replayed a completed foreign call")

print(("t-ffi-callxs-remote-flush OK: boxed GC/flush %.3f/%.3fs, " ..
       "bool GC/flush %.3f/%.3fs, sret GC/flush %.3f/%.3fs " ..
       "before release"):format(
      pointer_gc_elapsed, pointer_flush_elapsed,
      bool_gc_elapsed, bool_flush_elapsed,
      aggregate_gc_elapsed, aggregate_flush_elapsed))
