local th = require"threading"

local worker = th.spawn(function()
  local trace_count = require"jit_harness".trace_count
  local trace_limit = tonumber(os.getenv("LJ_M6_JIT_SECONDARY_TRACE_LIMIT")) or 512

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")

  local function branch(n, flag)
    local s = 0
    for i = 1, n do
      if flag and i == 10 then
	s = s + 1000
      else
	s = s + i
      end
    end
    return s
  end

  for _ = 1, 20 do
    assert(branch(80, false) == 3240)
  end
  local root_traces = trace_count(trace_limit)
  assert(root_traces > 0)

  for _ = 1, 20 do
    assert(branch(80, true) == 4230)
  end
  local side_traces = trace_count(trace_limit)
  assert(side_traces > root_traces)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local function table_alloc(n)
    local s = 0
    for i = 1, n do
      local t = { i }
      t.extra = i + 1
      s = s + t[1] + t.extra
    end
    return s
  end

  for _ = 1, 20 do
    assert(table_alloc(80) == 6560)
  end
  local table_traces = trace_count(trace_limit)
  assert(table_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared = { stable = 7, [3] = 11 }

  local function table_read(n)
    local s = 0
    for _ = 1, n do
      s = s + shared.stable + shared[3]
    end
    return s
  end

  for _ = 1, 20 do
    assert(table_read(80) == 1440)
  end
  local read_traces = trace_count(trace_limit)
  assert(read_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local with_index = setmetatable({}, { __index = { fallback = 13 } })

  local function table_index_read(n)
    local s = 0
    for _ = 1, n do
      s = s + with_index.fallback
    end
    return s
  end

  for _ = 1, 20 do
    assert(table_index_read(80) == 1040)
  end
  local index_traces = trace_count(trace_limit)
  assert(index_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared_write = { stable = 0, 0 }

  local function table_write(n)
    for i = 1, n do
      shared_write.stable = i
      shared_write[1] = i + 1
    end
    return shared_write.stable + shared_write[1]
  end

  for _ = 1, 20 do
    assert(table_write(80) == 161)
  end
  local write_traces = trace_count(trace_limit)
  assert(write_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local meta_hits = 0
  local shared_meta_write = setmetatable({ stable = 0, 0 }, {
    __newindex = function()
      meta_hits = meta_hits + 1
    end
  })

  local function table_meta_write(n)
    for i = 1, n do
      shared_meta_write.stable = i
      shared_meta_write[1] = i + 1
    end
    return shared_meta_write.stable + shared_meta_write[1]
  end

  for _ = 1, 20 do
    assert(table_meta_write(80) == 161)
    assert(meta_hits == 0)
  end
  local meta_write_traces = trace_count(trace_limit)
  assert(meta_write_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared_meta_nil_hash = setmetatable({ stable = 0 }, {})
  shared_meta_nil_hash.stable = nil

  local function table_meta_nil_hash_write(n)
    for i = 1, n do
      shared_meta_nil_hash.stable = i
      shared_meta_nil_hash.stable = nil
    end
    return shared_meta_nil_hash.stable
  end

  for _ = 1, 20 do
    assert(table_meta_nil_hash_write(80) == nil)
  end
  local meta_nil_hash_traces = trace_count(trace_limit)
  assert(meta_nil_hash_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared_meta_nil_array = setmetatable({ 0, nil, 0 }, {})

  local function table_meta_nil_array_write(n)
    for i = 1, n do
      shared_meta_nil_array[2] = i
      shared_meta_nil_array[2] = nil
    end
    return shared_meta_nil_array[2]
  end

  for _ = 1, 20 do
    assert(table_meta_nil_array_write(80) == nil)
  end
  local meta_nil_array_traces = trace_count(trace_limit)
  assert(meta_nil_array_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local insert_hits = 0
  local shared_meta_insert = setmetatable({}, {
    __newindex = function(_, _, v)
      insert_hits = insert_hits + v
    end
  })

  local function table_meta_insert_newindex(n)
    local before = insert_hits
    for i = 1, n do
      shared_meta_insert.missing = i
    end
    return insert_hits - before, rawget(shared_meta_insert, "missing")
  end

  for _ = 1, 20 do
    local hits, raw = table_meta_insert_newindex(80)
    assert(hits == 3240)
    assert(raw == nil)
  end

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared_ipairs = { 2, 4, 6, nil, 100 }

  local function table_ipairs(n)
    local s = 0
    for _ = 1, n do
      local count = 0
      local subtotal = 0
      for i, v in ipairs(shared_ipairs) do
	assert(i == count + 1)
	count = i
	subtotal = subtotal + v
      end
      assert(count == 3)
      s = s + subtotal
    end
    return s
  end

  for _ = 1, 20 do
    assert(table_ipairs(80) == 960)
  end
  local ipairs_traces = trace_count(trace_limit)
  assert(ipairs_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local function table_next(n)
    local s = 0
    for i = 1, n do
      local t = { only = i }
      local key, value = next(t, nil)
      assert(key == "only")
      s = s + value
    end
    return s
  end

  for _ = 1, 20 do
    assert(table_next(80) == 3240)
  end
  local next_traces = trace_count(trace_limit)
  assert(next_traces > 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local shared_next = { alpha = 3, beta = 5, gamma = 7 }

  local function shared_table_next(n)
    local s = 0
    for _ = 1, n do
      local count = 0
      local subtotal = 0
      local key = nil
      while true do
	local value
	key, value = next(shared_next, key)
	if key == nil then break end
	count = count + 1
	subtotal = subtotal + value
      end
      assert(count == 3)
      s = s + subtotal
    end
    return s
  end

  for _ = 1, 20 do
    assert(shared_table_next(80) == 1200)
  end
  local shared_next_traces = trace_count(trace_limit)
  assert(shared_next_traces == 0)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local function shared_table_pairs(n)
    local s = 0
    for _ = 1, n do
      local count = 0
      local subtotal = 0
      for _, value in pairs(shared_next) do
	count = count + 1
	subtotal = subtotal + value
      end
      assert(count == 3)
      s = s + subtotal
    end
    return s
  end

  for _ = 1, 20 do
    assert(shared_table_pairs(80) == 1200)
  end
  local shared_pairs_traces = trace_count(trace_limit)
  assert(shared_pairs_traces == 0)

  return root_traces, side_traces, table_traces, read_traces, index_traces,
	 write_traces, meta_write_traces, meta_nil_hash_traces,
	 meta_nil_array_traces, ipairs_traces, next_traces, shared_next_traces,
	 shared_pairs_traces, th.current():id()
end)

local ok, root_traces, side_traces, table_traces, read_traces, index_traces,
      write_traces, meta_write_traces, meta_nil_hash_traces,
      meta_nil_array_traces, ipairs_traces, next_traces, shared_next_traces,
      shared_pairs_traces, tid = worker:join()
assert(ok == true, tostring(root_traces))
assert(type(root_traces) == "number" and root_traces > 0)
assert(type(side_traces) == "number" and side_traces > root_traces)
assert(type(table_traces) == "number" and table_traces > 0)
assert(type(read_traces) == "number" and read_traces > 0)
assert(type(index_traces) == "number" and index_traces > 0)
assert(type(write_traces) == "number" and write_traces > 0)
assert(type(meta_write_traces) == "number" and meta_write_traces > 0)
assert(type(meta_nil_hash_traces) == "number" and meta_nil_hash_traces > 0)
assert(type(meta_nil_array_traces) == "number" and meta_nil_array_traces > 0)
assert(type(ipairs_traces) == "number" and ipairs_traces > 0)
assert(type(next_traces) == "number" and next_traces > 0)
assert(type(shared_next_traces) == "number" and shared_next_traces == 0)
assert(type(shared_pairs_traces) == "number" and shared_pairs_traces == 0)
assert(tid == worker:id())

print("t-jit-secondary OK: secondary TG records, enters, side-traces, allocates tables, reads/writes shared tables, traces existing and previous-nil metatable stores, records shared ipairs(), traces trace-local next(), keeps active-MT shared next()/pairs() interpreted, and preserves __index/__newindex semantics in x64 mcode")
