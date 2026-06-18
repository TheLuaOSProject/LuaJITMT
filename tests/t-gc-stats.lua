local function assert_number(t, k)
  assert(type(t[k]) == "number", k)
end

local function bucket_total(stats)
  local buckets = stats.poll_ack_latency_buckets
  local total = 0
  assert(type(buckets) == "table", "poll_ack_latency_buckets")
  assert(#buckets >= 32, "poll_ack_latency_buckets")
  for i = 1, #buckets do
    assert_number(buckets, i)
    assert(buckets[i] >= 0)
    total = total + buckets[i]
  end
  return total
end

local before = collectgarbage("stats")
assert(type(before) == "table")
assert(type(collectgarbage("count")) == "number")
assert(type(collectgarbage("isrunning")) == "boolean")
assert(type(collectgarbage("step", 0)) == "boolean")
assert_number(before, "total_bytes")
assert_number(before, "total_kbytes")
assert_number(before, "phase")
assert_number(before, "generational")
assert_number(before, "cycle_requests")
assert_number(before, "cycle_starts")
assert_number(before, "poll_ack_samples")
assert_number(before, "poll_ack_latency_sum_ns")
assert_number(before, "poll_ack_latency_max_ns")
local before_bucket_total = bucket_total(before)
assert(before_bucket_total == before.poll_ack_samples)
assert_number(before, "alloc_since_trigger")
assert_number(before, "trigger_bytes")
assert_number(before, "hard_bytes")
assert_number(before, "assist_runs")
assert_number(before, "assist_grey_drained")
assert_number(before, "assist_ssb_converted")
assert_number(before, "assist_weak_drained")
assert_number(before, "sweep_owner_runs")
assert_number(before, "sweep_owner_arenas")
assert_number(before, "sweep_owner_live_cells")
assert_number(before, "sweep_live_updates")
assert_number(before, "sweep_live_huge_bytes")
assert_number(before, "live_estimate")
assert_number(before, "weak_clear_tables")
assert_number(before, "weak_clear_cleared")
assert_number(before, "weak_legacy_fallbacks")
assert_number(before, "weak_legacy_backfills")
assert_number(before, "finalizer_queued")
assert_number(before, "finalizer_dequeued")

local keep = {}
for i = 1, 2000 do
  keep[i] = {i, i + 1, i + 2}
end
collectgarbage("collect")

local after = collectgarbage("stats")
assert(after.total_bytes > 0)
assert(after.total_kbytes >= 0)
assert(after.cycle_starts >= before.cycle_starts)
assert(after.poll_ack_samples >= before.poll_ack_samples)
assert(after.poll_ack_latency_sum_ns >= before.poll_ack_latency_sum_ns)
assert(after.poll_ack_latency_max_ns >= before.poll_ack_latency_max_ns)
local after_bucket_total = bucket_total(after)
assert(after_bucket_total == after.poll_ack_samples)
assert(after_bucket_total >= before_bucket_total)
assert(after.sweep_owner_runs >= before.sweep_owner_runs)
assert(after.sweep_live_updates >= before.sweep_live_updates)
assert(after.live_estimate >= 0)

print("t-gc-stats OK")
