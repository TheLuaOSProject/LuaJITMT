local ffi = require("ffi")

local M = {}

function M.force(prefix, count)
  count = tonumber(count) or 512
  prefix = prefix or "lj_test_ctype_growth"
  local parts = {}
  for i = 1, count do
    parts[#parts + 1] =
      ("typedef struct { int x; } %s_%d_t;\n"):format(prefix, i)
  end
  ffi.cdef(table.concat(parts))
end

return M
