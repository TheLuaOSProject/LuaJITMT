local threading = require("threading")

local worker_count = 4
local iterations = 900
local sizes = { 31, 4096, 65536 }
local alphabet = {
  { "a", "a", "A" }, { "B", "b", "B" },
  { "c", "c", "C" }, { "D", "d", "D" },
  { "e", "e", "E" }, { "F", "f", "F" },
  { "g", "g", "G" }, { "H", "h", "H" },
  { "0", "0", "0" }, { "7", "7", "7" },
  { "x", "x", "X" }, { "Y", "y", "Y" }
}

local function make_case(worker_id, size)
  local source, lower, upper, reverse = {}, {}, {}, {}
  for i = 1, size do
    local tuple = alphabet[((i * 7 + worker_id * 5) % #alphabet) + 1]
    source[i] = tuple[1]
    lower[i] = tuple[2]
    upper[i] = tuple[3]
  end

  -- Give every worker unmistakable ends without changing the requested size.
  local first = alphabet[worker_id]
  local last = alphabet[#alphabet - worker_id]
  source[1], lower[1], upper[1] = first[1], first[2], first[3]
  source[size], lower[size], upper[size] = last[1], last[2], last[3]
  for i = 1, size do
    reverse[i] = source[size - i + 1]
  end

  local item = {
    source = table.concat(source),
    reverse = table.concat(reverse),
    lower = table.concat(lower),
    upper = table.concat(upper)
  }
  assert(#item.source == size and #item.reverse == size)
  assert(#item.lower == size and #item.upper == size)
  return item
end

local cases = {}
for worker_id = 1, worker_count do
  cases[worker_id] = {}
  for i = 1, #sizes do
    cases[worker_id][i] = make_case(worker_id, sizes[i])
  end
end

local ready = threading.channel(worker_count)
local start = threading.channel(worker_count)
local workers = {}

for worker_id = 1, worker_count do
  workers[worker_id] = assert(threading.spawn(function(id, worker_cases,
                                                       ready_ch, start_ch,
                                                       count)
    assert(ready_ch:send(id) == true)
    local go, go_ok = start_ch:recv(15)
    assert(go_ok == true and go == "go", "worker start barrier failed")

    local retained = {}
    local total = 0
    for i = 1, count do
      local item = worker_cases[((i - 1) % #worker_cases) + 1]
      local reverse = string.reverse(item.source)
      local lower = string.lower(item.source)
      local upper = string.upper(item.source)
      assert(reverse == item.reverse, "thread-local reverse tmpbuf corrupted")
      assert(lower == item.lower, "thread-local lower tmpbuf corrupted")
      assert(upper == item.upper, "thread-local upper tmpbuf corrupted")

      local slot = ((i - 1) % 24) + 1
      retained[slot] = { reverse, lower, upper }
      total = total + #reverse + #lower + #upper
      if i % 29 == 0 then
        collectgarbage("collect")
      end
    end

    for i = 1, #retained do
      local triple = retained[i]
      assert(type(triple) == "table" and #triple == 3)
      assert(#triple[1] == #triple[2] and #triple[2] == #triple[3])
    end
    return id, total, #retained
  end, worker_id, cases[worker_id], ready, start, iterations))
end

local seen = {}
for _ = 1, worker_count do
  local worker_id, ok = ready:recv(15)
  assert(ok == true and type(worker_id) == "number", "ready barrier failed")
  assert(not seen[worker_id], "duplicate ready worker")
  seen[worker_id] = true
end
for _ = 1, worker_count do
  assert(start:send("go", 15) == true)
end

for worker_id = 1, worker_count do
  local ok, returned_id, total, retained = workers[worker_id]:join(60)
  assert(ok == true, "tmpbuf worker failed")
  assert(returned_id == worker_id, "tmpbuf worker identity changed")
  assert(type(total) == "number" and total > 0, "tmpbuf worker lost output")
  assert(retained == 24, "tmpbuf rotating retention changed")
end

print("arm64-tmpbuf-thread OK: reverse/lower/upper stayed TG-local")
