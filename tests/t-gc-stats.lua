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

local finreg_stats = {
  "weak_bridge_backfill_tables",
  "weak_bridge_backfill_slots",
  "weak_bridge_backfill_cleared",
  "weak_keys_marked",
  "weak_values_marked",
  "finreg_cdata_sets",
  "finreg_cdata_clears",
  "finreg_cdata_queued",
  "finreg_cdata_sweep_queued",
  "finreg_cdata_pweak_queued",
  "finreg_cdata_pweak_claimed",
  "finreg_cdata_preclaim_overflow",
  "finreg_cdata_preclaim_dispatched",
  "finreg_cdata_order_seen",
  "finreg_cdata_order_claimed",
  "finreg_cdata_order_unlinked",
  "finreg_cdata_order_queued",
  "finreg_cdata_order_retired",
  "finreg_cdata_order_tombstones",
  "finreg_cdata_order_fallbacks",
  "finreg_cdata_pending_order_hits",
  "finreg_udata_sets",
  "finreg_udata_clears",
  "finreg_udata_queued",
  "finreg_udata_registered",
  "finreg_udata_retired_nodes",
  "finreg_udata_discovered",
  "finreg_udata_forgets",
  "finalizer_mpsc_drained",
  "finalizer_enters",
  "finalizer_leaves",
  "finalizer_sweep_blocks",
  "finalizer_spawn_deferrals",
  "finalizer_spawn_release_wakes",
}

local before = collectgarbage("stats")
assert(type(before) == "table")
assert(type(collectgarbage("count")) == "number")
assert(type(collectgarbage("isrunning")) == "boolean")
assert(type(collectgarbage("step", 0)) == "boolean")
assert_number(before, "total_bytes")
assert_number(before, "total_kbytes")
assert_number(before, "phase")
assert_number(before, "generational")
assert_number(before, "cycle_minor_requested")
assert_number(before, "cycle_sweep_minor")
assert_number(before, "minor_sweep_enabled")
assert_number(before, "cycle_roots_minor")
assert_number(before, "minor_roots_enabled")
assert_number(before, "cycle_requests")
assert_number(before, "cycle_starts")
assert_number(before, "major_cycle_starts")
assert_number(before, "minor_cycle_requests")
assert_number(before, "minor_cycle_starts")
assert_number(before, "minor_sweep_deferred")
assert_number(before, "minor_sweep_arenas")
assert_number(before, "minor_roots_deferred")
assert_number(before, "major_root_scans")
assert_number(before, "minor_root_scans")
assert_number(before, "minor_survival_base_live")
assert_number(before, "minor_survival_bytes")
assert_number(before, "minor_survival_pct")
assert_number(before, "minor_survival_threshold_pct")
assert_number(before, "minor_survival_major_requests")
assert_number(before, "remembered_barriers")
assert_number(before, "remembered_pushed")
assert_number(before, "remembered_overflows")
assert_number(before, "remembered_filtered")
assert_number(before, "remembered_drained")
assert_number(before, "poll_ack_samples")
assert_number(before, "poll_ack_latency_sum_ns")
assert_number(before, "poll_ack_latency_max_ns")
local before_bucket_total = bucket_total(before)
assert(before_bucket_total == before.poll_ack_samples)
assert_number(before, "alloc_since_trigger")
assert_number(before, "cycle_alloc_bytes")
assert_number(before, "trigger_bytes")
assert_number(before, "hard_bytes")
assert_number(before, "assist_runs")
assert_number(before, "assist_grey_drained")
assert_number(before, "assist_ssb_converted")
assert_number(before, "assist_weak_drained")
assert_number(before, "worker_runs")
assert_number(before, "worker_grey_drained")
assert_number(before, "worker_ssb_converted")
assert_number(before, "worker_weak_drained")
assert_number(before, "worker_idle_declares")
assert_number(before, "worker_busy_retries")
assert_number(before, "worker_wakes")
assert_number(before, "worker_parks")
assert_number(before, "worker_async_progress")
assert_number(before, "sweep_owner_runs")
assert_number(before, "sweep_owner_arenas")
assert_number(before, "sweep_owner_live_cells")
assert_number(before, "sweep_live_updates")
assert_number(before, "sweep_live_huge_bytes")
assert_number(before, "live_estimate")
assert_number(before, "weak_clear_tables")
assert_number(before, "weak_clear_cleared")
assert_number(before, "weak_bridge_skipped")
assert_number(before, "weak_bridge_fallbacks")
assert_number(before, "weak_bridge_backfills")
assert_number(before, "finalizer_queued")
assert_number(before, "finalizer_dequeued")
for i = 1, #finreg_stats do
  assert_number(before, finreg_stats[i])
end

local keep = {}
for i = 1, 2000 do
  keep[i] = {i, i + 1, i + 2}
end
collectgarbage("collect")

local after = collectgarbage("stats")
assert(after.total_bytes > 0)
assert(after.total_kbytes >= 0)
assert(after.cycle_starts >= before.cycle_starts)
assert(after.major_cycle_starts >= before.major_cycle_starts)
assert(after.minor_cycle_requests >= before.minor_cycle_requests)
assert(after.minor_cycle_starts >= before.minor_cycle_starts)
assert(after.minor_sweep_deferred >= before.minor_sweep_deferred)
assert(after.minor_sweep_arenas >= before.minor_sweep_arenas)
assert(after.minor_roots_deferred >= before.minor_roots_deferred)
assert(after.major_root_scans >= before.major_root_scans)
assert(after.minor_root_scans >= before.minor_root_scans)
assert(after.minor_survival_pct >= 0)
assert(after.minor_survival_pct <= 100)
assert(after.minor_survival_threshold_pct >= 1)
assert(after.minor_survival_major_requests >= before.minor_survival_major_requests)
assert(after.remembered_barriers >= before.remembered_barriers)
assert(after.remembered_pushed >= before.remembered_pushed)
assert(after.remembered_overflows >= before.remembered_overflows)
assert(after.remembered_filtered >= before.remembered_filtered)
assert(after.remembered_drained >= before.remembered_drained)
assert(after.poll_ack_samples >= before.poll_ack_samples)
assert(after.poll_ack_latency_sum_ns >= before.poll_ack_latency_sum_ns)
assert(after.poll_ack_latency_max_ns >= before.poll_ack_latency_max_ns)
local after_bucket_total = bucket_total(after)
assert(after_bucket_total == after.poll_ack_samples)
assert(after_bucket_total >= before_bucket_total)
assert(after.worker_runs >= before.worker_runs)
assert(after.worker_grey_drained >= before.worker_grey_drained)
assert(after.worker_ssb_converted >= before.worker_ssb_converted)
assert(after.worker_weak_drained >= before.worker_weak_drained)
assert(after.worker_idle_declares >= before.worker_idle_declares)
assert(after.worker_busy_retries >= before.worker_busy_retries)
assert(after.worker_wakes >= before.worker_wakes)
assert(after.worker_parks >= before.worker_parks)
assert(after.worker_async_progress >= before.worker_async_progress)
assert(after.sweep_owner_runs >= before.sweep_owner_runs)
assert(after.sweep_live_updates >= before.sweep_live_updates)
assert(after.live_estimate >= 0)
assert(after.finalizer_queued >= before.finalizer_queued)
assert(after.finalizer_dequeued >= before.finalizer_dequeued)
assert(after.weak_bridge_skipped >= before.weak_bridge_skipped)
for i = 1, #finreg_stats do
  local key = finreg_stats[i]
  assert_number(after, key)
  assert(after[key] >= before[key], key)
end

print("t-gc-stats OK")
