-- Raw capture promotion must follow loop-carried SSA values. Scope-closing
-- imports must retain branch liveness and each freshly created cell identity.
local root = os.getenv('LJ_TEST_ROOT') or '.'
local tracing = jit and jit.status()
local native = tracing and jit.arch == 'x64'
local fixture = root .. '/tests/fixtures/legacy-bytecode-cell-loops'

for _, suffix in ipairs({'.lua', '.bc', '-be.bc'}) do
  if tracing then
    jit.flush()
    jit.opt.start('hotloop=1', 'hotexit=1')
  end
  local iterator, repeated, branch = assert(loadfile(fixture .. suffix))()
  local roots, exits = {}, 0
  local function trace(what, tr, fn)
    if what == 'start' and (fn == iterator or fn == repeated) then
      roots[tr] = true
    elseif what == 'abort' then
      roots[tr] = nil
    end
  end
  local function exit(tr)
    if roots[tr] then exits = exits + 1 end
  end
  if native then
    jit.attach(trace, 'trace')
    jit.attach(exit, 'texit')
  end
  for round = 1, 40 do
    local values = iterator()
    for k = 1, 8 do
      assert(type(values[k]) == 'function', 'missing captured iterator key')
      local ck, cv = values[k]()
      assert(ck == k and cv == k + 10)
      if k > 1 then
        assert(debug.upvalueid(values[k], 1) ~= debug.upvalueid(values[k-1], 1))
      end
    end
    values = repeated()
    for k = 1, 8 do
      assert(values[k](10) == k + 10 and values[k]() == k + 10)
    end
    -- Keeping calls inside assert is significant: temporary argument/result
    -- slots are reused across the branch which skips the potential capture.
    for i = 1, 160 do
      assert(branch(i, true) == i + 1)
      assert(branch(i, false) == i)
    end
  end
  if native then
    jit.attach(trace)
    jit.attach(exit)
    assert(exits > 0, 'captured loops never executed native code')
  end
end
print('t-jit-cell-loops OK: source/import loop cells and conditional captures')
