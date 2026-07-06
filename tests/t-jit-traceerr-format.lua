local function upvalue(fn, name)
  for i = 1, 64 do
    local n, v = debug.getupvalue(fn, i)
    if n == name then return i, v end
    if n == nil then break end
  end
  error("missing upvalue: " .. name)
end

local function setupvalue(fn, name, value)
  local i = upvalue(fn, name)
  assert(debug.setupvalue(fn, i, value) == name)
end

local function probe(name, mod, dumpmode)
  local chunks = {}
  local _, dump_trace = upvalue(mod.on, "dump_trace")
  setupvalue(mod.on, "active", true)
  setupvalue(mod.on, "out", {
    write = function(_, ...)
      for i = 1, select("#", ...) do
	chunks[#chunks + 1] = tostring(select(i, ...))
      end
    end,
    flush = function() end,
  })
  if dumpmode then setupvalue(mod.on, "dumpmode", dumpmode) end

  local function marker() end
  dump_trace("start", 1, marker, 0)
  dump_trace("abort", 1, marker, 0, 7, nil)

  local out = table.concat(chunks)
  assert(out:match("NYI: bytecode %?"), name .. " output: " .. out)
end

probe("jit.v", require("jit.v"))
probe("jit.dump", require("jit.dump"), { t = true })

print("t-jit-traceerr-format OK: NYI bytecode trace diagnostics tolerate missing info")
