local M = {}

local Test = {}
Test.__index = Test

local function getenv(name, default)
  local v = os.getenv(name)
  if v == nil or v == "" then return default end
  return v
end

local function shell_quote(s)
  s = tostring(s)
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

local function detect_jobs()
  local p = io.popen("getconf _NPROCESSORS_ONLN 2>/dev/null")
  if p then
    local n = p:read("*l")
    p:close()
    n = tonumber(n)
    if n and n > 0 then return tostring(n) end
  end
  return "2"
end

local function append(parts, value)
  if value == nil or value == "" then return end
  parts[#parts + 1] = value
end

local function append_argv(parts, argv)
  for i = 1, #argv do
    append(parts, shell_quote(argv[i]))
  end
end

local function read_file(path)
  local f, err = io.open(path, "rb")
  if not f then error(path .. ": " .. err, 2) end
  local data = f:read("*a")
  f:close()
  return data
end

function M.new(root)
  local self = {
    root = root,
    compiler = getenv("CC", "cc"),
    cflags = getenv("CFLAGS", "-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"),
    jobs = getenv("JOBS", getenv("MAKE_JOBS", detect_jobs())),
    tmpdir = getenv("TMPDIR", "/tmp")
  }
  return setmetatable(self, Test)
end

function Test:path(...)
  local out = self.root
  for i = 1, select("#", ...) do
    out = out .. "/" .. select(i, ...)
  end
  return out
end

function Test:tmp(name)
  return self.tmpdir .. "/" .. name
end

function Test:read(path)
  return read_file(path)
end

function Test:run(cmd, opts)
  opts = opts or {}
  local parts = {}
  if opts.env then
    for k, v in pairs(opts.env) do
      append(parts, k .. "=" .. shell_quote(v))
    end
  end
  if opts.timeout then
    append(parts, "timeout " .. shell_quote(opts.timeout))
  end
  if type(cmd) == "table" then
    append_argv(parts, cmd)
  else
    append(parts, cmd)
  end
  local full = table.concat(parts, " ")
  if not opts.quiet then
    io.stderr:write("+ " .. full .. "\n")
  end
  local ok, why, code = os.execute(full)
  local success
  if type(ok) == "number" then
    success = ok == 0
    code = ok
  else
    success = ok == true and (code == nil or code == 0)
  end
  if not success then
    error("command failed (" .. tostring(code or why or ok) .. "): " .. full, 2)
  end
end

function Test:make(args, opts)
  opts = opts or {}
  local cmd = { "make", "-C", self:path("src") }
  if opts.jobs ~= false then
    cmd[#cmd + 1] = "-j" .. self.jobs
  end
  if args then
    for i = 1, #args do cmd[#cmd + 1] = args[i] end
  end
  self:run(cmd, opts)
end

function Test:build(opts)
  opts = opts or {}
  if opts.clean then
    self:make({ "clean" }, { quiet = opts.quiet, jobs = false })
  end
  local args = {}
  if opts.xcflags then args[#args + 1] = "XCFLAGS=" .. opts.xcflags end
  self:make(args, { quiet = opts.quiet })
end

function Test:cc(output, sources, opts)
  opts = opts or {}
  local parts = { self.compiler, self.cflags }
  append(parts, "-I" .. shell_quote(self:path("src")))
  for i = 1, #sources do
    append(parts, shell_quote(sources[i]))
  end
  if opts.objects then
    for i = 1, #opts.objects do append(parts, shell_quote(opts.objects[i])) end
  end
  if opts.link_luajit then
    append(parts, shell_quote(self:path("src", "libluajit.a")))
  end
  if opts.libs then
    for i = 1, #opts.libs do append(parts, opts.libs[i]) end
  end
  append(parts, "-o " .. shell_quote(output))
  self:run(table.concat(parts, " "), { quiet = opts.quiet })
end

function Test:luajit(args, opts)
  opts = opts or {}
  local argv = { self:path("src", "luajit") }
  for i = 1, #args do argv[#argv + 1] = args[i] end
  self:run(argv, opts)
end

function Test:assert_contains(path, needle)
  local data = read_file(path)
  if not data:find(needle, 1, true) then
    error(path .. ": missing expected text: " .. needle, 2)
  end
end

function Test:assert_not_contains(path, needle)
  local data = read_file(path)
  if data:find(needle, 1, true) then
    error(path .. ": forbidden text present: " .. needle, 2)
  end
end

function Test:assert_ordered(path, needles)
  local data = read_file(path)
  local pos = 1
  for i = 1, #needles do
    local next_pos = data:find(needles[i], pos, true)
    if not next_pos then
      error(path .. ": missing expected text: " .. needles[i], 2)
    end
    pos = next_pos + #needles[i]
  end
end

return M
