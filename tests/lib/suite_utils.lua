local M = {}

function M.getenv(name, default)
  local v = os.getenv(name)
  if v == nil or v == "" then return default end
  return v
end

function M.shell_quote(s)
  s = tostring(s)
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

function M.contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

function M.count_plain(s, needle)
  local count, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return count end
    count = count + 1
    pos = last + 1
  end
end

function M.lines(s)
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then out[#out + 1] = line end
  end
  return out
end

function M.iter_lines(s)
  return (s .. "\n"):gmatch("(.-)\n")
end

function M.as_list(v)
  if type(v) == "string" then return { v } end
  return v
end

function M.line_hits(t, paths, pred)
  local hits = {}
  paths = M.as_list(paths)
  for i = 1, #paths do
    local path = paths[i]
    local n = 0
    for line in M.iter_lines(t:read(path)) do
      n = n + 1
      if pred(line, path, n) then
        hits[#hits + 1] = path .. ":" .. n .. ": " .. line
      end
    end
  end
  return hits
end

function M.assert_no_lines(t, label, paths, pred)
  local hits = M.line_hits(t, paths, pred)
  if #hits > 0 then
    error(label .. ":\n" .. table.concat(hits, "\n"), 2)
  end
end

function M.escape_pattern(s)
  return (s:gsub("([^%w_])", "%%%1"))
end

function M.has_ident(s, ident)
  return s:find("%f[%w_]" .. M.escape_pattern(ident) .. "%f[^%w_]") ~= nil
end

function M.line_contains_any(line, needles)
  for i = 1, #needles do
    if M.contains(line, needles[i]) then return true end
  end
  return false
end

function M.assert_text_not_contains(label, data, needle)
  if M.contains(data, needle) then
    error(label .. ": forbidden text present: " .. needle, 2)
  end
end

function M.append_list(dst, src)
  for i = 1, #src do dst[#dst + 1] = src[i] end
  return dst
end

function M.read_file(path)
  local f, err = io.open(path, "rb")
  if not f then error(path .. ": " .. err, 2) end
  local data = f:read("*a")
  f:close()
  return data
end

function M.file_exists(path)
  local f = io.open(path, "rb")
  if f then f:close(); return true end
  return false
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

function M.run_output_contains(t, cmd, needle)
  t:run(cmd .. " | rg -F " .. M.shell_quote(needle) .. " >/dev/null")
end

function M.run_output_contains_all(t, cmd, needles)
  for i = 1, #needles do
    cmd = cmd .. " | rg -F " .. M.shell_quote(needles[i])
  end
  t:run(cmd .. " >/dev/null")
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

return M
