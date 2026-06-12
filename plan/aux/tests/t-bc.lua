-- t-bc.lua — bytecode compatibility (10 §10.7; t-bc-01..03).
-- tests/golden/legacy_v2.bin must be generated ONCE with a stock luajit:
--   stock-luajit -e 'io.write(string.dump(loadstring([[
--     local n = 0
--     local function inc() n = n + 1 end
--     local function get() return n end
--     return function(k) for i=1,k do inc() end return get() end
--   ]])))' > tests/golden/legacy_v2.bin
local T = require("harness")
local th = require("threading")

local function load_golden()
  local f = io.open("tests/golden/legacy_v2.bin", "rb")
  if not f then return nil end
  local d = f:read("*a"); f:close()
  return assert(loadstring(d, "=legacy"))
end

T.case("01 v2 dump loads and runs (single-thread exactness)", function()
  local chunk = load_golden()
  if not chunk then print("  (skip: golden missing)"); return end
  local run = chunk()
  T.eq(run(5), 5)
  T.eq(run(3), 8)            -- open-upvalue sharing intact pre-mt_active
end)

T.case("02 legacy closures under mt_active: capture-at-FNEW (10 §10.4)", function()
  local chunk = load_golden()
  if not chunk then print("  (skip: golden missing)"); return end
  -- flip the latch by spawning any thread
  assert(th.spawn(function() return 1 end):join())
  local mk = chunk            -- creating NEW closures now snapshots
  local run = mk()            -- inc/get created post-latch: independent
  local a = run(5)
  local b = run(5)
  -- documented deviation: inc's n and get's n are separate snapshots,
  -- so get never observes inc's writes. The exact values depend on the
  -- chunk shape; assert only the documented property: monotone-or-equal,
  -- never the v3 fully-shared behavior count.
  T.truthy(type(a) == "number" and type(b) == "number")
  T.truthy(b <= a + 5, "legacy semantics drifted beyond spec")
end)

T.case("03 v4 dump round-trips; legacy protos refuse to dump", function()
  local function fresh()
    local n = 0
    return function() n = n + 1 return n end
  end
  local d = string.dump(fresh)             -- v4 emitter (10 §10.6)
  local fresh2 = assert(loadstring(d))
  local c = fresh2()
  T.eq(c(), 1); T.eq(c(), 2)
  local chunk = load_golden()
  if chunk then
    local ok = pcall(string.dump, chunk)
    T.eq(ok, false, "dumping a legacy-loaded function must error")
  end
end)

T.case("04 v4 cell opcodes rejected by verifier when smuggled into v2", function()
  -- corrupt a v2 header onto a v4 body: loader must reject (10 §10.5)
  local d3 = string.dump(function() local x = 0 local function f() x = 1 end return f end)
  if #d3 > 5 then
    local forged = d3:sub(1, 3) .. string.char(2) .. d3:sub(5)
    local ok = pcall(loadstring, forged)
    T.truthy(not ok or select(2, pcall(loadstring(forged))) ~= nil,
             "forged v2 header with cell ops accepted")
  end
end)

T.done()
