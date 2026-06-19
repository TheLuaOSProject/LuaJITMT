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

function M.command_ok(cmd)
  local ok, _, code = os.execute(cmd .. " >/dev/null 2>&1")
  if type(ok) == "number" then return ok == 0 end
  return ok == true and (code == nil or code == 0)
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
