local th = require"threading"

local worker = th.spawn(function()
  local ffi = require"ffi"

  ffi.cdef[[
  typedef struct { int x; double y; } lj_m7_snap_restore_t;
  ]]

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")

  local struct_t = ffi.typeof("lj_m7_snap_restore_t")
  local int64_t = ffi.typeof("int64_t")

  local function make_struct(n, stop)
    for i = 1, n do
      local obj = struct_t(i, i + 0.5)
      if i == stop then
        return obj
      end
    end
    return nil
  end

  local function make_i64(n, stop)
    for i = 1, n do
      local v = int64_t(i)
      if i == stop then
        return v
      end
    end
    return nil
  end

  for _ = 1, 60 do
    assert(make_struct(80, 0) == nil)
    assert(make_i64(80, 0) == nil)
  end

  local obj = make_struct(80, 37)
  assert(obj.x == 37)
  assert(obj.y == 37.5)

  local v = make_i64(80, 41)
  assert(tonumber(v) == 41)

  collectgarbage("collect")
  collectgarbage("collect")
  return true
end)

local ok, result = worker:join(30)
assert(ok == true, tostring(result))
assert(result == true)

print("t-ffi-snap-restore-l OK")
