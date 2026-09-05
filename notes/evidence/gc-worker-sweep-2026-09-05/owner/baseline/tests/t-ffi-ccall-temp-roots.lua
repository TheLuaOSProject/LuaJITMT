local ffi = require"ffi"

jit.off()
jit.flush()

ffi.cdef[[
typedef struct lj_m7_ccall_root_blob {
  uint32_t lane[16];
} lj_m7_ccall_root_blob;
typedef int (*lj_m7_ccall_root_cb)(uint32_t cookie);
lj_m7_ccall_root_blob lj_m7_ccall_root_roundtrip(
  lj_m7_ccall_root_blob input, lj_m7_ccall_root_cb callback,
  uint32_t cookie);
]]

local lib = ffi.load(assert(os.getenv("LJ_M7_FFI_CCALL_ROOT_SO")))
local blob_t = ffi.typeof("lj_m7_ccall_root_blob")
local cb_t = ffi.typeof("lj_m7_ccall_root_cb")
local cookie = 0x12345
local callback_count = 0

local input = blob_t()
for i = 0, 15 do input.lane[i] = 1000 + i * 37 end

local function full_gc_and_reuse()
  collectgarbage("collect")
  collectgarbage("collect")
  local keep = {}
  for i = 1, 1024 do
    local v = blob_t()
    v.lane[0] = 0x7000 + i
    keep[i] = v
  end
  keep = nil
  collectgarbage("collect")
  collectgarbage("collect")
end

local callback = ffi.cast(cb_t, function(got_cookie)
  assert(tonumber(got_cookie) == cookie)
  callback_count = callback_count + 1
  full_gc_and_reuse()
  return 19
end)

local function invoke(cb)
  return lib.lj_m7_ccall_root_roundtrip(input, cb, cookie)
end

local function pack(...)
  return select("#", ...), ...
end

for round = 1, 12 do
  local nresult, output = pack(invoke(callback))
  assert(nresult == 1, "temporary C-call roots escaped into result shape")
  for i = 0, 15 do
    local source = 1000 + i * 37
    local expected = bit.band(bit.bxor(source, 0x005a5a5a) + 19 + i,
                              0xffffffff)
    assert(tonumber(output.lane[i]) == expected,
           ("round %d lane %d: got %s expected %s"):format(
             round, i, tostring(output.lane[i]), tostring(expected)))
    assert(tonumber(input.lane[i]) == source, "by-value input was corrupted")
  end
end
assert(callback_count == 12)

-- Callback errors unwind the ordinary Lua-stack roots together with the call
-- frame. A subsequent collected call must retain the exact one-result shape.
local bad_callback = ffi.cast(cb_t, function()
  full_gc_and_reuse()
  error("intentional temporary-root callback error")
end)
local ok, err = pcall(invoke, bad_callback)
assert(ok == false and tostring(err):find("intentional temporary%-root"))
bad_callback:free()
full_gc_and_reuse()

local nresult, final = pack(invoke(callback))
assert(nresult == 1 and tonumber(final.lane[0]) ==
       bit.band(bit.bxor(1000, 0x005a5a5a) + 19, 0xffffffff))
assert(callback_count == 13)

callback:free()
print("t-ffi-ccall-temp-roots OK: aggregate ABI temporaries survive callback GC")
