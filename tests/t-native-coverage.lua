local utils = require('suite_utils')
local root = os.getenv('LJ_TEST_ROOT') or '.'
local workload = os.tmpname()
local command = utils.shell_quote(root .. '/src/luajit') .. ' ' ..
                utils.shell_quote(root .. '/bench/native-coverage.lua') .. ' '
local function run(source, exits_only)
  utils.write_file(workload, source)
  return utils.capture_command(command .. (exits_only and '--exits-only ' or '') ..
    utils.shell_quote(workload), {stderr = true, timeout = '30s'})
end
local ok, err = xpcall(function()
  -- Explicit interpreter control, followed by a real compiled numeric loop.
  local out = run([[jit.off(); print('workload result', 42)]])
  assert(out:find('workload result%s+42'))
  assert(out:find('published=0 aborted=0 observed_exits=0'))
  out = run([[jit.off(); print('workload result', 42)]], true)
  assert(out:find('mode=exits%-only trace_observer=off observed_exits=0'))
  assert(not out:find('published=', 1, true))
  if jit.opt then
    local source = [[
      jit.flush()
      jit.opt.start('hotloop=2', 'hotexit=1000')
      local kernel = assert(loadstring('return function(n) local s=0.5;' ..
        'local i=1; while i<=n do s=s+i; i=i+1 end; return s end'))()
      local function driver()
        for _=1,20 do assert(kernel(1024)==524800.5) end
      end
      jit.off(driver)
      driver()
    ]]
    out = run(source)
    assert(tonumber(out:match('published=(%d+)')) > 0, out)
    assert(tonumber(out:match('observed_exits=(%d+)')) > 0, out)
    assert(out:match('trace=%d+ source=.- ir=%d+ bytes=%d+'), out)
    out = run(source, true)
    assert(out:find('trace_observer=off', 1, true), out)
    assert(tonumber(out:match('observed_exits=(%d+)')) > 0, out)
    assert(out:match('trace_number=%d+ observed_exits=%d+'), out)
    assert(not out:find('published=', 1, true), out)
  end
  local accepted, message = pcall(run,
    "jit.attach(function() end, 'trace')", true)
  assert(not accepted and tostring(message):find('coverage incomplete:', 1, true))
  -- A workload must not silently replace the observer and report false zeros.
  for _, event in ipairs({'trace', 'texit'}) do
    local accepted, message = pcall(run,
      "jit.attach(function() end, '" .. event .. "')")
    assert(not accepted and tostring(message):find('coverage incomplete:', 1, true))
    assert(tostring(message):find('native-coverage owns jit.attach', 1, true))
  end
  local accepted, message = pcall(run, "error('failed workload')")
  assert(not accepted and tostring(message):find('coverage incomplete:', 1, true))
end, debug.traceback)
os.remove(workload)
assert(ok, err)
print('t-native-coverage OK: runtime observations, interpreter control and hook ownership')
