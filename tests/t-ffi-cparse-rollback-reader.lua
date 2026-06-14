local ffi = require("ffi")
local th = require("threading")

ffi.cdef("struct lj_m7_rollback_reader;")

local ct = ffi.typeof("struct lj_m7_rollback_reader")
local ctid = tonumber(ct)
local backing = ffi.new("char[8]")
local p = ffi.cast("struct lj_m7_rollback_reader *", backing)

local function bad_cdef_source(tag)
  local parts = { "struct lj_m7_rollback_reader { int x; };\n" }
  for i = 1, 20000 do
    parts[#parts + 1] =
      ("typedef int lj_m7_rollback_reader_pad_%s_%d;\n"):format(tag, i)
  end
  parts[#parts + 1] = "@\n"
  return table.concat(parts)
end

local function race_failed_cdef(tag, check)
  local done = th.channel(1)
  th.spawn(function(done_ch, src)
    local ffi = require("ffi")
    local ok = pcall(ffi.cdef, src)
    done_ch:send(ok and "ok" or "err")
  end, done, bad_cdef_source(tag))

  local result
  while true do
    local msg, ok = done:recv(0)
    if ok == true then
      result = msg
      break
    end
    check()
  end
  assert(result == "err", result)
end

race_failed_cdef("direct", function()
  assert(ffi.sizeof(ct) == nil,
	 "direct ctype reader observed failed cdef rollback state")
  assert(ffi.typeinfo(ctid).size == nil,
	 "ffi.typeinfo observed failed cdef rollback state")
  assert(not pcall(ffi.new, ct, { x = 123 }),
	 "ffi.new observed failed cdef rollback state")
end)

race_failed_cdef("index", function()
  assert(not pcall(function() return p.x end),
	 "cdata __index observed failed cdef rollback state")
end)

race_failed_cdef("newindex", function()
  assert(not pcall(function() p.x = 123 end),
	 "cdata __newindex observed failed cdef rollback state")
end)

race_failed_cdef("numindex", function()
  assert(not pcall(function() return p[0] end),
	 "cdata numeric __index observed failed cdef rollback state")
end)

assert(ffi.sizeof(ct) == nil, "failed cdef left incomplete struct completed")
assert(ffi.typeinfo(ctid).size == nil,
       "failed cdef left typeinfo for incomplete struct completed")
assert(not pcall(ffi.new, ct, { x = 123 }),
       "failed cdef left ffi.new able to allocate incomplete struct")
assert(not pcall(function() return p.x end),
       "failed cdef left cdata __index able to read incomplete struct")
assert(not pcall(function() p.x = 123 end),
       "failed cdef left cdata __newindex able to write incomplete struct")
assert(not pcall(function() return p[0] end),
       "failed cdef left cdata numeric __index able to read incomplete struct")

print("t-ffi-cparse-rollback-reader OK: direct ctype/typeinfo/new/field/numeric readers wait out rollback")
