local th = require 'threading'

function control_tnew()
  local t = {}
  assert(type(t) == 'table')
end
function control_tdup()
  local t = { 23, 47 }
  assert(t[1] + t[2] == 70)
end
function control_fastfunc()
  assert(string.char(65) == 'A')
end
function control_cfunc()
  assert(string.rep('x', 2) == 'xx')
end
function control_fnew()
  local f = function(x) return x + 1 end
  assert(f(41) == 42)
end
local stopped_ring = {}
function control_stopped_alloc()
  for i = 1, 256 do stopped_ring[i % 32 + 1] = { i } end
end
function control_native()
  th.sleep(0.002)
end

-- Warm the same library paths before any measured request is published.
control_tnew()
control_tdup()
control_fastfunc()
control_cfunc()
control_fnew()
control_stopped_alloc()
control_native()
