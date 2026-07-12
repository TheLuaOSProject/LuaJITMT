local pieces = {}

pieces[#pieces + 1] = "return function(seed)\nlocal value = seed\n"
for i = 1, 900 do
  local name = ("parser_identifier_%04d_"):format(i) .. string.rep("x", 80)
  pieces[#pieces + 1] =
    ("do local %s = %d\nvalue = value + %s end\n"):format(name, i, name)
  if i % 90 == 0 then
    pieces[#pieces + 1] = "do local captured = value; " ..
      "value = (function() return captured end)() end\n"
  end
end
pieces[#pieces + 1] = "return value\nend\n"
local source = table.concat(pieces)
pieces = nil

local in_bc = false
local bc_events = 0
local function bc_hook()
  bc_events = bc_events + 1
  if in_bc then return end
  in_bc = true
  for _ = 1, 2 do
    local garbage = {}
    for i = 1, 300 do garbage[i] = { tostring(i), i } end
    collectgarbage("collect")
  end
  in_bc = false
end
jit.attach(bc_hook, "bc")

local function load_chunked(text, fail_at)
  local pos, calls = 1, 0
  return load(function()
    calls = calls + 1
    if fail_at and calls == fail_at then
      error("intentional reader failure")
    end
    if pos > #text then return nil end
    -- Reentrant source loads exercise the LexState descriptor LIFO while the
    -- outer reader owns no Lua-stack representation of its raw buffers.
    if calls % 17 == 0 then
      local nested = assert(loadstring("return 40 + 2", "=(nested-parser)"))
      assert(nested() == 42)
    end
    collectgarbage("collect")
    local chunk = text:sub(pos, pos + 4095)
    pos = pos + #chunk
    return chunk
  end, "=" .. string.rep("long-parser-chunkname-", 80))
end

local compiled, err = load_chunked(source)
assert(compiled, err)
local run = compiled()
assert(type(run) == "function")
assert(run(3) == 3 + (900 * 901) / 2)
assert(bc_events > 5, bc_events)

-- Syntax errors after all three raw buffers have grown must unwind both the
-- semantic proto anchors and the native LexState root descriptor.
for i = 1, 5 do
  local bad, message = load_chunked(source .. "\nfunction broken(")
  assert(bad == nil and type(message) == "string", i)
  collectgarbage("collect")
end

-- A reader error happens during lj_lex_setup's first read, after descriptor
-- publication but before normal setup return.
local ok, failed, message = pcall(load_chunked, source, 1)
assert(ok == true and failed == nil and
       tostring(message):find("intentional reader failure", 1, true))

local final = assert(loadstring("return 'parser-root-clean'", "=(after-errors)"))
collectgarbage("collect")
assert(final() == "parser-root-clean")
jit.attach(bc_hook)

print("t-parser-gc2-roots.lua OK: forced GC, BC events, nested readers, and errors")
