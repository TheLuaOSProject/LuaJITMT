local th = require"threading"

local marker = assert(arg and arg[1], "missing marker path")
local spin_marker = assert(arg and arg[2], "missing spin marker path")
local ch = th.channel(0)
local ready = th.channel(1)
local spin_ready = th.channel(1)

th.spawn(function(q, r, path)
  r:send(true)
  local ok, err = pcall(function()
    q:recv()
  end)
  local f = assert(io.open(path, "w"))
  f:write(tostring(ok), "\n", tostring(err), "\n")
  f:close()
end, ch, ready, marker)

local v, ok = ready:recv(1)
assert(v == true and ok == true)

th.spawn(function(r, path)
  r:send(true)
  local ok, err = pcall(function()
    local n = 0
    while true do
      n = n + 1
      if n > 1000000 then n = 0 end
    end
  end)
  local f = assert(io.open(path, "w"))
  f:write(tostring(ok), "\n", tostring(err), "\n")
  f:close()
end, spin_ready, spin_marker)

local sv, sok = spin_ready:recv(1)
assert(sv == true and sok == true)

-- Return without joining. lua_close must request STOPREQ, interrupt the
-- parked and CPU-bound workers, wait for them to finish, and only then let the
-- process exit.
