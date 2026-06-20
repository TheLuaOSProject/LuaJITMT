local ffi = require"ffi"
local harness = require"thread_harness"

ffi.cdef[[
typedef struct { int x; } lj_m7_pin_obj_t;
]]

local pin_obj_t = ffi.typeof("lj_m7_pin_obj_t")
local fullgc = harness.fullgc

do
  local weak = setmetatable({}, { __mode = "v" })
  local obj = { tag = "rooted" }
  local pin = ffi.pin(obj)
  assert(tostring(pin) == "ffi.pin")
  weak[1] = obj
  obj = nil
  fullgc()
  assert(weak[1] ~= nil, "pin must keep Lua object alive")
  pin:release()
  pin:release()
  fullgc()
  assert(weak[1] == nil, "released pin must drop Lua object")
end

do
  local finalized = 0
  local function fin(_)
    finalized = finalized + 1
  end
  local cd = ffi.gc(pin_obj_t(), fin)
  local pin = ffi.pin(cd)
  cd = nil
  fullgc()
  assert(finalized == 0, "pin must keep cdata finalizer pending")
  pin:release()
  fullgc()
  assert(finalized == 1, "released pin must allow cdata finalizer")
end

do
  local finalized = 0
  local function fin(_)
    finalized = finalized + 1
  end
  local cd = ffi.gc(pin_obj_t(), fin)
  local pin = ffi.pin(cd)
  cd = nil
  pin = nil
  fullgc()
  fullgc()
  assert(finalized == 1, "pin __gc must release the hidden root")
end

do
  local ok, err = pcall(function() ffi.pin() end)
  assert(ok == false and tostring(err):match("bad argument #1"))
  local pin = ffi.pin(nil)
  pin:release()
end

do
  local ok, th = pcall(require, "threading")
  if ok then
    local nthreads = harness.arg_number(1, "LJ_M7_FFI_PIN_THREADS", 4)
    local iters = harness.arg_number(2, "LJ_M7_FFI_PIN_ITERS", 80)
    if nthreads > 0 and iters > 0 then
      local ready, start = harness.channels(nthreads)
      local workers = {}

      for tid = 1, nthreads do
	workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
	  local ffi = require"ffi"
	  pcall(ffi.cdef, [[typedef struct { int x; } lj_m7_pin_obj_t;]])
	  local pin_obj_t = ffi.typeof("lj_m7_pin_obj_t")
	  local fullgc = require"thread_harness".fullgc

	  local function table_pin_once(i)
	    local weak = setmetatable({}, { __mode = "v" })
	    local obj = { id = i }
	    local pin = ffi.pin(obj)
	    weak[1] = obj
	    obj = nil
	    fullgc()
	    assert(weak[1] ~= nil)
	    pin:release()
	    return weak
	  end

	  ready_ch:send(id)
	  local token, started = start_ch:recv(10)
	  assert(started == true and token == "go")

	  for i = 1, count do
	    table_pin_once(i)

	    local cd = pin_obj_t()
	    local cpin = ffi.pin(cd)
	    cd = nil
	    cpin:release()
	  end
	  return count
	end, ready, start, tid, iters)
	harness.wait_ready(ready, 1)
      end

      harness.release_start(start, nthreads)
      local total = harness.join_count(workers)
      assert(total == nthreads * iters)
    end
  end
end

print("t-ffi-pin OK")
