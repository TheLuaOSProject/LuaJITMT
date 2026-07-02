local ffi = require("ffi")
local th = require("threading")

ffi.cdef("struct lj_m7_rollback_reader;")
ffi.cdef("struct lj_m7_rollback_ctor;")
ffi.cdef("enum lj_m7_rollback_enum;")

local ct = ffi.typeof("struct lj_m7_rollback_reader")
local ctid = tonumber(ct)
local ctor_ct = ffi.typeof("struct lj_m7_rollback_ctor")
local ctor_ptr_ct = ffi.typeof("struct lj_m7_rollback_ctor *")
local enum_ct = ffi.typeof("enum lj_m7_rollback_enum")
local backing = ffi.new("char[16]")
local p = ffi.cast("struct lj_m7_rollback_reader *", backing)
local q = ffi.cast("struct lj_m7_rollback_reader *",
		   ffi.cast("char *", backing) + 4)

local function bad_cdef_source(tag)
  local parts = { "struct lj_m7_rollback_reader { int x; };\n" }
  for i = 1, 20000 do
    parts[#parts + 1] =
      ("typedef int lj_m7_rollback_reader_pad_%s_%d;\n"):format(tag, i)
  end
  parts[#parts + 1] = "@\n"
  return table.concat(parts)
end

local function bad_ctor_const_cdef_source(tag)
  local parts = {
    "struct lj_m7_rollback_ctor {\n"
      .. "static const int K = 55;\n"
      .. "enum { E = 56 };\n"
      .. "int x;\n"
      .. "};\n"
  }
  for i = 1, 20000 do
    parts[#parts + 1] =
      ("typedef int lj_m7_rollback_ctor_pad_%s_%d;\n"):format(tag, i)
  end
  parts[#parts + 1] = "@\n"
  return table.concat(parts)
end

local function bad_enum_cdef_source(tag)
  local parts = { "enum lj_m7_rollback_enum { LJ_M7_ROLLBACK_ENUM_TMP = 17 };\n" }
  for i = 1, 20000 do
    parts[#parts + 1] =
      ("typedef int lj_m7_rollback_enum_pad_%s_%d;\n"):format(tag, i)
  end
  parts[#parts + 1] = "@\n"
  return table.concat(parts)
end

local function bad_const_cdef_source(tag)
  local parts = { "static const int lj_m7_rollback_tmp_const = 123;\n" }
  for i = 1, 20000 do
    parts[#parts + 1] =
      ("typedef int lj_m7_rollback_const_pad_%s_%d;\n"):format(tag, i)
  end
  parts[#parts + 1] = "@\n"
  return table.concat(parts)
end

local function race_failed_cdef(tag, check, source)
  local done = th.channel(1)
  th.spawn(function(done_ch, src)
    local ffi = require("ffi")
    local ok = pcall(ffi.cdef, src)
    done_ch:send(ok and "ok" or "err")
  end, done, (source or bad_cdef_source)(tag))

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

local function assert_incomplete_typeinfo(msg)
  local ti = ffi.typeinfo(ctid)
  assert(ti ~= nil and ti.size == nil, msg)
end

race_failed_cdef("direct", function()
  assert(ffi.sizeof(ct) == nil,
	 "direct ctype reader observed failed cdef rollback state")
  assert_incomplete_typeinfo(
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

race_failed_cdef("ptradd", function()
  assert(not pcall(function() return p + 1 end),
	 "cdata pointer add observed failed cdef rollback state")
end)

race_failed_cdef("ptrdiff", function()
  assert(not pcall(function() return q - p end),
	 "cdata pointer diff observed failed cdef rollback state")
end)

race_failed_cdef("cast", function()
  assert(not pcall(function()
    return ffi.cast("struct lj_m7_rollback_reader *", backing).x
  end), "ffi.cast observed failed cdef rollback state")
end)

race_failed_cdef("ctorconst", function()
  assert(not pcall(function() return ctor_ct.K end),
	 "ctype constructor observed failed cdef rollback constant")
  assert(not pcall(function() return ctor_ptr_ct.K end),
	 "pointer constructor observed failed cdef rollback constant")
  assert(not pcall(function() return ctor_ct.E end),
	 "ctype constructor observed failed cdef rollback enum constant")
  assert(not pcall(function() return ctor_ct.x end),
	 "ctype constructor observed failed cdef rollback field")
end, bad_ctor_const_cdef_source)

race_failed_cdef("enumcast", function()
  assert(not pcall(ffi.cast, enum_ct, "LJ_M7_ROLLBACK_ENUM_TMP"),
	 "enum string cast observed failed cdef rollback state")
end, bad_enum_cdef_source)

race_failed_cdef("enumcastnum", function()
  assert(not pcall(ffi.cast, enum_ct, 17),
	 "enum numeric cast observed failed cdef rollback state")
end, bad_enum_cdef_source)

race_failed_cdef("ffic", function()
  assert(not pcall(function() return ffi.C.lj_m7_rollback_tmp_const end),
	 "ffi.C observed failed cdef rollback constant")
end, bad_const_cdef_source)

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
assert(not pcall(function() return p + 1 end),
       "failed cdef left cdata pointer add able to step incomplete struct")
assert(not pcall(function() return q - p end),
       "failed cdef left cdata pointer diff able to size incomplete struct")
assert(not pcall(function()
  return ffi.cast("struct lj_m7_rollback_reader *", backing).x
end), "failed cdef left ffi.cast able to read incomplete struct field")
assert(not pcall(function() return ctor_ct.K end),
       "failed cdef left ctype constructor able to see rolled-back constant")
assert(not pcall(function() return ctor_ptr_ct.K end),
       "failed cdef left pointer constructor able to see rolled-back constant")
assert(not pcall(function() return ctor_ct.E end),
       "failed cdef left ctype constructor able to see rolled-back enum constant")
assert(not pcall(function() return ctor_ct.x end),
       "failed cdef left ctype constructor able to see rolled-back field")
assert(not pcall(ffi.cast, enum_ct, "LJ_M7_ROLLBACK_ENUM_TMP"),
       "failed cdef left enum string cast able to see rolled-back constant")
assert(not pcall(ffi.cast, enum_ct, 17),
       "failed cdef left enum numeric cast able to use rolled-back layout")
assert(not pcall(function() return ffi.C.lj_m7_rollback_tmp_const end),
       "failed cdef left ffi.C able to see rolled-back constant")

print("t-ffi-cparse-rollback-reader OK: direct ctype/typeinfo/new/field/numeric/ptrarith/constructor/namespace readers wait out rollback")
