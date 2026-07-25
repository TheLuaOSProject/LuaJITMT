-- SIMD test suite runner.
--
--   luajit test/simd/run.lua [-jit|-interp|-mixed] [name ...]
--
-- SIMD_SEED picks the random seed. Run more than one seed before believing
-- the suite: a wrong guard polarity in a mask predicate only showed up on
-- some seeds.
--
-- Each (file, mode) pair runs in its own process so that ffi.cdef state and
-- compiled traces from one test cannot influence another.
local dir = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = dir .. "/?.lua;" .. package.path

local ALL = {"test_types", "test_arith", "test_lib", "test_jit", "test_codegen",
	     "test_ffi_abi"}

-- Child process: run one file in one mode.
if arg[1] == "--one" then
  local mode, name = arg[2], arg[3]
  local jit_ = require("jit")
  -- jit.off()/on() with no arguments switch the whole engine, which also
  -- covers functions loaded after this point. "mixed" leaves the engine on
  -- but disables the already loaded functions, which produces a very
  -- different (and historically bug-finding) set of traces.
  if mode == "interp" then jit_.off()
  elseif mode == "mixed" then jit_.off(true, true)
  else jit_.on() end
  local T = assert(loadfile(dir .. "/" .. name .. ".lua"))()
  os.exit(T.run(name .. " [" .. mode .. "]") and 0 or 1)
end

local files, mode = {}, "both"
for _, a in ipairs(arg) do
  if a == "-jit" then mode = "jit"
  elseif a == "-interp" then mode = "interp"
  elseif a == "-mixed" then mode = "mixed"
  else files[#files+1] = a end
end
if #files == 0 then files = ALL end

local luajit = arg[-1] or "luajit"
local modes = mode == "both" and {"interp", "jit", "mixed"} or {mode}
local failed = false

for _, m in ipairs(modes) do
  for _, f in ipairs(files) do
    local fh = io.open(dir .. "/" .. f .. ".lua")
    if fh then
      fh:close()
      local cmd = string.format("%s %q --one %s %s", luajit, arg[0], m, f)
      local ok = os.execute(cmd)
      if ok ~= 0 and ok ~= true then failed = true end
    end
  end
end

if failed then
  io.write("SIMD TESTS FAILED\n")
  os.exit(1)
end
io.write("ALL SIMD TESTS PASSED\n")
