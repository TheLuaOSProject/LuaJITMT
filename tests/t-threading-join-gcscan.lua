local th = require"threading"

local reps = tonumber(os.getenv("LJ_M4_JOIN_GCSCAN_REPS") or "300")

local function make_closed_gc_cell(label)
  local value = { seq = 0, payload = { label } }
  local function store(v)
    value = v
    return value
  end
  local function load()
    return value
  end
  return store, load
end

for i = 1, reps do
  local store, load = make_closed_gc_cell("join-gcscan")
  local parent = { seq = i, payload = { "parent", i } }
  store(parent)

  local worker = th.spawn(function(n)
    collectgarbage("collect")
    local before = load()
    assert(before.seq == n and before.payload[1] == "parent")
    store({ seq = n + 1, payload = { before.payload[1], "worker", n } })
    collectgarbage("collect")
    local after = load()
    return after.seq == n + 1 and after.payload[2] == "worker"
  end, i)

  local ok, done = worker:join()
  assert(ok == true and done == true, i)
  collectgarbage("collect")
  assert(load().seq == i + 1 and load().payload[2] == "worker", i)
end

print("t-threading-join-gcscan OK")
