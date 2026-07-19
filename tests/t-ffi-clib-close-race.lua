local ffi = require"ffi"
local th = require"threading"

local so = assert(os.getenv("LJ_M7_FFI_CLIB_CLOSE_SO"))

ffi.cdef[[
extern int lj_m7_clib_snapshot_value;
int lj_m7_clib_symbol_that_does_not_exist(void);
void lj_clib_test_publish_pause(void);
unsigned int lj_clib_test_publish_paused(void);
void lj_clib_test_publish_release(void);
void lj_clib_test_counters_reset(void);
unsigned int lj_clib_test_retired_handles(void);
unsigned int lj_clib_test_native_closes(void);
]]

-- Resolve the test controls through ffi.C before arming the publication hook.
-- Otherwise the control lookup itself could be the first paused cache fill.
local pause = ffi.C.lj_clib_test_publish_pause
local paused = ffi.C.lj_clib_test_publish_paused
local release = ffi.C.lj_clib_test_publish_release
local reset = ffi.C.lj_clib_test_counters_reset
local retired = ffi.C.lj_clib_test_retired_handles
local native_closes = ffi.C.lj_clib_test_native_closes

local cl = ffi.load(so)
local gc = assert(debug.getmetatable(cl).__gc)

-- Both declared-missing and undeclared failures cross the protected miss
-- body. A leaked reader would make the close race below strand cleanup.
assert(not pcall(function() return cl.lj_m7_clib_symbol_that_does_not_exist end))
assert(not pcall(function() return cl.lj_m7_clib_undeclared_name end))

reset()
pause()
local worker = th.spawn(function(shared)
  local ok, err = pcall(function()
    return shared.lj_m7_clib_snapshot_value
  end)
  return not ok and tostring(err):find("closed C library", 1, true) ~= nil
end, cl)

local deadline = th.now() + 10
while paused() == 0 and th.now() < deadline do
  th.sleep(0.001)
end
assert(paused() ~= 0, "CLibrary publisher did not reach the pause hook")

-- Both calls must return without waiting for the admitted publisher. The
-- second call is an idempotent loser, not a second cache detach/handle retire.
gc(cl)
gc(cl)
assert(retired() == 0, "handle retired before the admitted reader left")
assert(native_closes() == 0, "native handle closed during runtime")

release()
local joined, safe = worker:join(10)
assert(joined == true and safe == true)
assert(retired() == 1, "close did not publish exactly one retired handle")
assert(native_closes() == 0, "native handle closed before joined-world teardown")

local ok, err = pcall(function()
  return cl.lj_m7_clib_snapshot_value
end)
assert(not ok and tostring(err):find("closed C library", 1, true))
gc(cl)
assert(retired() == 1, "repeated __gc published another retired handle")

print("t-ffi-clib-close-race OK: close is non-waiting, idempotent and trace-safe")
