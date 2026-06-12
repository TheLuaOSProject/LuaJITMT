local th = require"threading"

local marker = assert(arg and arg[1], "missing marker path")
local ch = th.channel(0)
local ready = th.channel(1)

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

-- Return without joining. lua_close must request STOPREQ, interrupt the
-- parked worker, wait for it to finish, and only then let the process exit.
