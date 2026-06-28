local M = {}
local checks = require("suite_assert")

function M.getenv(name, default)
  local v = os.getenv(name)
  if v == nil or v == "" then return default end
  return v
end

function M.shell_quote(s)
  s = tostring(s)
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

function M.append_list(dst, src)
  for i = 1, #src do dst[#dst + 1] = src[i] end
  return dst
end

function M.case_registry(add)
  local cases = {}
  local function register(test)
    cases[test.name] = test
    add(test)
  end
  return cases, register
end

function M.run_case(cases, t, name, args)
  local test = cases[name]
  if not test then error("unknown test: " .. tostring(name), 2) end
  io.stderr:write("== " .. name .. " ==\n")
  test.run(t, args or {})
  io.stderr:write("ok " .. name .. "\n")
end

function M.run_cases(cases, t, names)
  for i = 1, #names do
    M.run_case(cases, t, names[i])
  end
end

local function read_raw_file(path)
  local f, err = io.open(path, "rb")
  if not f then error(path .. ": " .. err, 2) end
  local data = f:read("*a")
  f:close()
  return data
end

function M.read_file(path)
  checks.assert_not_source_file_content(path, 2)
  return read_raw_file(path)
end

function M.write_file(path, data, mode)
  local f, err = io.open(path, mode or "wb")
  if not f then error(path .. ": " .. err, 2) end
  local ok, werr = f:write(data)
  f:close()
  if not ok then error(path .. ": " .. tostring(werr), 2) end
  return path
end

function M.file_exists(path)
  local f = io.open(path, "rb")
  if f then f:close(); return true end
  return false
end

function M.with_temp_paths(t, prefixes, fn)
  local paths = {}
  for i = 1, #prefixes do
    paths[i] = t:tempname(prefixes[i])
    t:remove(paths[i])
  end
  local results = { pcall(fn, unpack(paths)) }
  for i = 1, #paths do t:remove(paths[i]) end
  if not results[1] then error(results[2], 0) end
  return unpack(results, 2)
end

function M.has_extension(path, extensions)
  if not extensions then return true end
  if type(extensions) == "string" then
    return path:sub(-#extensions) == extensions
  end
  for i = 1, #extensions do
    local ext = extensions[i]
    if path:sub(-#ext) == ext then return true end
  end
  return false
end

function M.detect_jobs()
  local p = io.popen("getconf _NPROCESSORS_ONLN 2>/dev/null")
  if p then
    local n = p:read("*l")
    p:close()
    n = tonumber(n)
    if n and n > 0 then
      if n > 2 then n = 2 end
      return tostring(n)
    end
  end
  return "2"
end

function M.command_succeeded(cmd)
  local ok, _, code = os.execute(cmd)
  if type(ok) == "number" then return ok == 0 end
  return ok == true and (code == nil or code == 0)
end

function M.command_ok(cmd)
  return M.command_succeeded(cmd .. " >/dev/null 2>&1")
end

function M.capture_command(cmd, opts)
  opts = opts or {}
  local full = cmd
  if opts.stderr then full = full .. " 2>&1" end
  if opts.timeout then
    full = "timeout " .. M.shell_quote(opts.timeout) .. " sh -c " ..
           M.shell_quote(full)
  end
  local p, err = io.popen(full)
  if not p then error("command failed to start: " .. tostring(err), 2) end
  local out = p:read("*a")
  local ok, why, code = p:close()
  if not ok then
    error("command failed (" .. tostring(code or why or ok) .. "): " ..
          full .. "\n" .. out, 2)
  end
  return out
end

function M.assert_command_output_contains(cmd, needle, opts)
  local out = M.capture_command(cmd, opts)
  checks.assert_text_contains(cmd, out, needle, "command output")
  return out
end

function M.assert_command_output_all_contains(cmd, needles, opts)
  local out = M.capture_command(cmd, opts)
  checks.assert_text_all_contains(cmd, out, needles, "command output")
  return out
end

function M.assert_command_fails(cmd)
  if M.command_succeeded(cmd) then
    error("command unexpectedly succeeded: " .. cmd, 2)
  end
end

function M.capture_lines(cmd)
  local p, err = io.popen(cmd)
  if not p then error("command failed to start: " .. tostring(err), 2) end
  local out = {}
  for line in p:lines() do out[#out + 1] = line end
  local ok, why, code = p:close()
  if not ok then
    error("command failed (" .. tostring(code or why or ok) .. "): " .. cmd, 2)
  end
  return out
end

function M.lua_path(root)
  return root .. "/src/?.lua;" .. root .. "/src/jit/?.lua;;"
end

function M.luajit_fixture_libs(opts)
  if opts and opts.libs then return opts.libs end
  local libs = { "-lm", "-ldl" }
  if not opts or opts.pthread ~= false then
    libs[#libs + 1] = opts and opts.pthread or "-pthread"
  end
  return libs
end

return M
