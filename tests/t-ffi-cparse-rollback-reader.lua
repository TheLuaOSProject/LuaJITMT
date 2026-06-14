local ffi = require("ffi")
local th = require("threading")

ffi.cdef("struct lj_m7_rollback_reader;")

local ct = ffi.typeof("struct lj_m7_rollback_reader")
local parts = { "struct lj_m7_rollback_reader { int x; };\n" }
for i = 1, 20000 do
  parts[#parts + 1] = ("typedef int lj_m7_rollback_reader_pad_%d;\n"):format(i)
end
parts[#parts + 1] = "@\n"

local done = th.channel(1)
th.spawn(function(done_ch, src)
  local ffi = require("ffi")
  local ok = pcall(ffi.cdef, src)
  done_ch:send(ok and "ok" or "err")
end, done, table.concat(parts))

local result
while true do
  local msg, ok = done:recv(0)
  if ok == true then
    result = msg
    break
  end
  assert(ffi.sizeof(ct) == nil,
	 "direct ctype reader observed failed cdef rollback state")
end

assert(result == "err", result)
assert(ffi.sizeof(ct) == nil, "failed cdef left incomplete struct completed")

print("t-ffi-cparse-rollback-reader OK: direct ctype readers wait out rollback")
