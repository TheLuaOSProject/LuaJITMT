local ffi = require"ffi"

ffi.cdef[[
typedef struct { int x; } lj_m7_ptrid_base_t;
typedef lj_m7_ptrid_base_t lj_m7_ptrid_alias_t;
]]

local ct = ffi.metatype("lj_m7_ptrid_alias_t", {
  __tostring = function(self)
    return ("ptrid:%d"):format(tonumber(self.x))
  end,
})

local obj = ct(7)
assert(ffi.istype("lj_m7_ptrid_base_t", obj))
assert(ffi.istype("lj_m7_ptrid_alias_t", obj))
assert(tostring(obj) == "ptrid:7")

local arr = ffi.new("lj_m7_ptrid_base_t[1]", { { 13 } })
local ptr = ffi.cast("lj_m7_ptrid_base_t *", arr)
assert(ffi.istype("lj_m7_ptrid_base_t", ptr))
assert(tostring(ptr) == "ptrid:13")

print("t-ffi-ctype-pointer-ids OK")
