[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
0x00007ffbece137b9 in syscall () from /lib/x86_64-linux-gnu/libc.so.6
#0  0x00007ffbece137b9 in syscall () from /lib/x86_64-linux-gnu/libc.so.6
#1  0x000055c6a5d2ded4 in lj_gc2_sweep_prepare_bridge_boundary.part ()
#2  0x000055c6a5d41506 in lj_gc2_collect_active ()
#3  0x000055c6a5d9b549 in api_gc_collect_cp ()
#4  0x000055c6a5d0085b in lj_vm_cpcall_asm ()
#5  0x000055c6a5d71040 in lj_vm_cpcall ()
#6  0x000055c6a5da5e26 in lua_gc ()
#7  0x000055c6a5cf9ae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7ffbeccf00a0, g=g@entry=0x7ffbeccf0150, tg=tg@entry=0x7ffbeccf2d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
#8  0x000055c6a5cf5f64 in main () at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:3126
#7  0x000055c6a5cf9ae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7ffbeccf00a0, g=g@entry=0x7ffbeccf0150, tg=tg@entry=0x7ffbeccf2d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
2967	    lua_gc(L, LUA_GCCOLLECT, 0);
$1 = {
  phase = 3,
  jit_phase_gate = 1,
  cycle = 6,
  thread_scan_cycle = 6,
  activation = {
    value = {
      lo = 6,
      hi = 612
    }
  },
  cycle_leader = 0,
  hs_epoch = 65,
  hs_pending = 0,
  hs_actions = 32,
  hs_leader = 0,
  hs_signal_ns = 11554773626615,
  hs_ack_latency_samples = 0,
  hs_ack_latency_sum_ns = 0,
  hs_ack_latency_max_ns = 0,
  hs_ack_latency_buckets = {0 <repeats 48 times>},
  smr_reclaim_runs = 3,
  smr_reclaimed = 66,
  smr_readers = 0,
  smr_reclaiming = 0,
  cycle_requests = 6,
  cycle_starts = 6,
  major_cycle_starts = 6,
  minor_cycle_requests = 0,
  minor_cycle_starts = 0,
  cycle_minor_requested = 0,
  cycle_sweep_minor = 0,
  minor_sweep_enabled = 0,
  cycle_roots_minor = 0,
  minor_roots_enabled = 0,
  minor_sweep_deferred = 0,
  minor_sweep_arenas = 0,
  minor_roots_deferred = 0,
  major_root_scans = 12,
  minor_root_scans = 0,
  pending_root_flushes = 6,
  pending_root_flushed = 293,
  pending_root_flush_max = 219,
  minor_survival_base_live = 27760,
  minor_survival_bytes = 0,
  minor_survival_pct = 0,
  minor_survival_threshold_pct = 80,
  minor_survival_major_requests = 0,
  force_major = 0,
  remembered_barriers = 0,
  remembered_pushed = 0,
  remembered_overflows = 0,
  remembered_filtered = 0,
  remembered_drained = 0,
  marks_this_round = 0,
  mark_root_scanned = 0,
  jit_mark_resume = 0,
  jit_mark_auto_yield = 0,
  jit_mark_yield_until_ns = 0,
  jit_sweep_displaced = 0,
  jit_sweep_yield_until_ns = 0,
  small_arena_tab = 0x55c6bcfa72f0,
  ssb_head = 0x0,
  ssb_drain = 0x0,
  ssb_consumer_active = 0,
  ssb_published = 19,
  ssb_drained = 19,
  ssb_items_published = 1271,
  ssb_items_drained = 1271,
  recovery_items = 0,
  recovery_huge_items = 0,
  recovery_published = 8,
  recovery_redirtied = 0,
  recovery_drained = 8,
  recovery_main_state = 0,
  recovery_failed = 0,
  recovery_scan_lane = 33,
  recovery_small_slot = 26277,
  recovery_small_cell = 730,
  recovery_huge_slot = 0,
  fixpoint_rounds = 21,
  fixpoint_hits = 4,
  mark_complete_runs = 17,
  mark_complete_hits = 4,
  mark_complete_peer_waits = 0,
  mark_to_weak = 4,
  weak_complete_runs = 16,
  weak_complete_progress = 4,
  weak_to_sweep = 4,
  sweep_bridge_ready = 1,
  sweep_root_scanned = 1,
  sweep_root_cursor = 0x0,
  sweep_root_done = 1,
  sweep_grace_needed = 0,
  sweep_to_idle = 3,
  preserve_abort_to_idle = 2,
  alloc_total_bytes = 168873,
  alloc_since_trigger = 0,
  cycle_alloc_bytes = 51232,
  trigger_bytes = 361546,
  hard_bytes = 723092,
  hard_check_bytes = 723092,
  helper_soft_limit = 18446744073709551615,
  assist_runs = 0,
  assist_grey_drained = 0,
  assist_ssb_converted = 0,
  assist_weak_drained = 0,
  jit_hard_checks = 0,
  interp_hard_checks = 0,
  jit_scoped_slots_retired = 0,
  clib_cache_retired = 0x0,
  clib_handle_retired = 0x0,
  gcpause_pct = 200,
  assist_shift = 1,
  assist_active = 0,
  generational = 0,
  grey_stack = 0x7ffbec9b00a0,
  grey_capacity = 4096,
  grey_top = 11,
  grey_bottom = 11,
  grey_pushed = 3318,
  grey_drained = 3287,
  grey_embedded = {{
      gcptr64 = 140719984037392
    }, {
      gcptr64 = 140719984037248
    }, {
      gcptr64 = 140719984182992
    }, {
      gcptr64 = 140719984182192
    }, {
      gcptr64 = 140719984182288
    }, {
      gcptr64 = 140719984182384
    }, {
      gcptr64 = 140719984182240
    }, {
      gcptr64 = 140719984181568
    }, {
      gcptr64 = 140719984180512
    }, {
      gcptr64 = 140719984180608
    }, {
      gcptr64 = 140719984180704
    }, {
      gcptr64 = 140719984180800
    }, {
      gcptr64 = 140719984180896
    }, {
      gcptr64 = 140719984180992
    }, {
      gcptr64 = 140719984181088
    }, {
      gcptr64 = 140719984181184
    }, {
      gcptr64 = 140719984181280
    }, {
      gcptr64 = 140719984181376
    }, {
      gcptr64 = 140719984181472
    }, {
      gcptr64 = 140719984179904
    }, {
      gcptr64 = 140719984180000
    }, {
      gcptr64 = 140719984180096
    }, {
      gcptr64 = 140719984179136
    }, {
      gcptr64 = 140719984179184
    }, {
      gcptr64 = 140719984179232
    }, {
      gcptr64 = 140719984179280
    }, {
      gcptr64 = 140719984180240
    }, {
      gcptr64 = 140719984180192
    }, {
      gcptr64 = 140719984179040
    }, {
      gcptr64 = 140719984179328
    }, {
      gcptr64 = 140719984179424
    }, {
      gcptr64 = 140719984179520
    }, {
      gcptr64 = 140719984179616
    }, {
      gcptr64 = 140719984178160
    }, {
      gcptr64 = 140719984178256
    }, {
      gcptr64 = 140719984178352
    }, {
      gcptr64 = 140719984178448
    }, {
      gcptr64 = 140719984178640
    }, {
      gcptr64 = 140719984178752
    }, {
      gcptr64 = 140719984175760
    }, {
      gcptr64 = 140719984175856
    }, {
      gcptr64 = 140719984175952
    }, {
      gcptr64 = 140719984176048
    }, {
      gcptr64 = 140719984176144
    }, {
      gcptr64 = 140719984176240
    }, {
      gcptr64 = 140719984176336
    }, {
      gcptr64 = 140719984176432
    }, {
      gcptr64 = 140719984176528
    }, {
      gcptr64 = 140719984176624
    }, {
      gcptr64 = 140719984176720
    }, {
      gcptr64 = 140719984176816
    }, {
      gcptr64 = 140719984176912
    }, {
      gcptr64 = 140719984177008
    }, {
      gcptr64 = 140719984177104
    }, {
      gcptr64 = 140719984177200
    }, {
      gcptr64 = 140719984177296
    }, {
      gcptr64 = 140719984177392
    }, {
      gcptr64 = 140719984177680
    }, {
      gcptr64 = 140719984177920
    }, {
      gcptr64 = 140719984177968
    }, {
      gcptr64 = 140719984178064
    }, {
      gcptr64 = 140719984173760
    }, {
      gcptr64 = 140719984174560
    }, {
      gcptr64 = 140719984174656
    }, {
      gcptr64 = 140719984174752
    }, {
      gcptr64 = 140719984174848
    }, {
      gcptr64 = 140719984174944
    }, {
      gcptr64 = 140719984175040
    }, {
      gcptr64 = 140719984175136
    }, {
      gcptr64 = 140719984175232
    }, {
      gcptr64 = 140719984174032
    }, {
      gcptr64 = 140719984174080
    }, {
      gcptr64 = 140719984174176
    }, {
      gcptr64 = 140719984174272
    }, {
      gcptr64 = 140719984174368
    }, {
      gcptr64 = 140719984174464
    }, {
      gcptr64 = 140719984172704
    }, {
      gcptr64 = 140719984172848
    }, {
      gcptr64 = 140719984172944
    }, {
      gcptr64 = 140719984173040
    }, {
      gcptr64 = 140719984173136
    }, {
      gcptr64 = 140719984173232
    }, {
      gcptr64 = 140719984173328
    }, {
      gcptr64 = 140719984173424
    }, {
      gcptr64 = 140719984173520
    }, {
      gcptr64 = 140719984172800
    }, {
      gcptr64 = 140719984173616
    }, {
      gcptr64 = 140719984171712
    }, {
      gcptr64 = 140719984171760
    }, {
      gcptr64 = 140719984171808
    }, {
      gcptr64 = 140719984172096
    }, {
      gcptr64 = 140719984172416
    }, {
      gcptr64 = 140719984171424
    }, {
      gcptr64 = 140719984171520
    }, {
      gcptr64 = 140719984171616
    }, {
      gcptr64 = 140719984171904
    }, {
      gcptr64 = 140719984172000
    }, {
      gcptr64 = 140719984172192
    }, {
      gcptr64 = 140719984172304
    }, {
      gcptr64 = 140719984172144
    }, {
      gcptr64 = 140719984171856
    }, {
      gcptr64 = 140719984170576
    }, {
      gcptr64 = 140719984170672
    }, {
      gcptr64 = 140719984170768
    }, {
      gcptr64 = 140719984170912
    }, {
      gcptr64 = 140719984171008
    }, {
      gcptr64 = 140719984171104
    }, {
      gcptr64 = 140719984171248
    }, {
      gcptr64 = 140719984171200
    }, {
      gcptr64 = 140719984170864
    }, {
      gcptr64 = 140719984175408
    }, {
      gcptr64 = 140719984183088
    }, {
      gcptr64 = 140719984183184
    }, {
      gcptr64 = 140719984182624
    }, {
      gcptr64 = 140719984170208
    }, {
      gcptr64 = 140719984170304
    }, {
      gcptr64 = 140719984182720
    }, {
      gcptr64 = 140719984168208
    }, {
      gcptr64 = 140719984168560
    }, {
      gcptr64 = 140719984168800
    }, {
      gcptr64 = 140719984168848
    }, {
      gcptr64 = 140719984168944
    }, {
      gcptr64 = 140719984169472
    }, {
      gcptr64 = 140719984169968
    }, {
      gcptr64 = 140719984170016
    }, {
      gcptr64 = 140719984170112
    }, {
      gcptr64 = 140719984166480
    }, {
      gcptr64 = 140719984166528
    }, {
      gcptr64 = 140719984166576
    }, {
      gcptr64 = 140719984166624
    }, {
      gcptr64 = 140719984166064
    }, {
      gcptr64 = 140719984166160
    }, {
      gcptr64 = 140719984166256
    }, {
      gcptr64 = 140719984161488
    }, {
      gcptr64 = 140719984165936
    }, {
      gcptr64 = 140719984165520
    }, {
      gcptr64 = 140719984165616
    }, {
      gcptr64 = 140719984165040
    }, {
      gcptr64 = 140719984165136
    }, {
      gcptr64 = 140719984165232
    }, {
      gcptr64 = 140719984165328
    }, {
      gcptr64 = 140719984165424
    }, {
      gcptr64 = 140719984162352
    }, {
      gcptr64 = 140719984162448
    }, {
      gcptr64 = 140719984162608
    }, {
      gcptr64 = 140719984162720
    }, {
      gcptr64 = 140719984162816
    }, {
      gcptr64 = 140719984162912
    }, {
      gcptr64 = 140719984163008
    }, {
      gcptr64 = 140719984163104
    }, {
      gcptr64 = 140719984163200
    }, {
      gcptr64 = 140719984163296
    }, {
      gcptr64 = 140719984163392
    }, {
      gcptr64 = 140719984044816
    }, {
      gcptr64 = 140719984044768
    }, {
      gcptr64 = 140719984044608
    }, {
      gcptr64 = 140719984044432
    }, {
      gcptr64 = 140719984044240
    }, {
      gcptr64 = 140719984044160
    }, {
      gcptr64 = 140719984044080
    }, {
      gcptr64 = 140719984044000
    }, {
      gcptr64 = 140719984043952
    }, {
      gcptr64 = 140719984043280
    }, {
      gcptr64 = 140719984043856
    }, {
      gcptr64 = 140719984043808
    }, {
      gcptr64 = 140719984043648
    }, {
      gcptr64 = 140719984043472
    }, {
      gcptr64 = 140719984043280
    }, {
      gcptr64 = 140719984043200
    }, {
      gcptr64 = 140719984043120
    }, {
      gcptr64 = 140719984043040
    }, {
      gcptr64 = 140719984182000
    }, {
      gcptr64 = 140719984182000
    }, {
      gcptr64 = 140719984042992
    }, {
      gcptr64 = 140719984042896
    }, {
      gcptr64 = 140719984042944
    }, {
      gcptr64 = 140719984042800
    }, {
      gcptr64 = 140719984042704
    }, {
      gcptr64 = 140719984042608
    }, {
      gcptr64 = 140719984042512
    }, {
      gcptr64 = 140719984042416
    }, {
      gcptr64 = 140719984042320
    }, {
      gcptr64 = 140719984042224
    }, {
      gcptr64 = 140719984042128
    }, {
      gcptr64 = 140719984042032
    }, {
      gcptr64 = 140719984041936
    }, {
      gcptr64 = 140719984041840
    }, {
      gcptr64 = 140719984041744
    }, {
      gcptr64 = 140719984041648
    }, {
      gcptr64 = 140719984041552
    }, {
      gcptr64 = 140719984041456
    }, {
      gcptr64 = 140719984041360
    }, {
      gcptr64 = 140719984041264
    }, {
      gcptr64 = 140719984041168
    }, {
      gcptr64 = 140719984041072
    }, {
      gcptr64 = 140719984040976
    }, {
      gcptr64 = 140719984040880
    }, {
      gcptr64 = 140719984040784
    }, {
      gcptr64 = 140719984040688
    }, {
      gcptr64 = 140719984040592
    }...},
  worker_thread = {0x0, 0x0},
  worker_tg = {0x0, 0x0},
  worker_tg_retired = 0x0,
  n_workers = 0,
  worker_control = 0,
  worker_stop = 0,
  worker_wake = 0,
  worker_started = 0,
  worker_exited = 0,
  worker_active = 0,
  mark_close_intent = 0,
  worker_runs = 3963970,
  worker_grey_drained = 42,
  worker_ssb_converted = 25,
  worker_weak_drained = 0,
  worker_idle_declares = 3,
  worker_busy_retries = 27,
  worker_wakes = 0,
  worker_parks = 0,
  worker_async_progress = 0,
  deferred_epoch = 0,
  tg_thread_roots = 12,
  tg_cur_roots = 0,
  tg_trace_roots = 0,
  thread_scan_claims = 16,
  thread_scan_busy = 0,
  thread_scan_requeues = 0,
  thread_scan_owner_scans = 0,
  thread_scan_needscan = 0,
  thread_scan_owner_needscans = 0,
  table_rescan_desc = {
    value = {
      lo = 0,
      hi = 0
    }
  },
  table_token_topology = {
    value = {
      lo = 10,
      hi = 0
    }
  },
  table_token_scan_requested = 0,
  table_token_small_slot = 0,
  table_token_small_cell = 618,
  table_token_huge_node = 0x0,
  table_token_huge_incarnation = 0,
  table_token_huge_slot = 0,
  table_token_huge_pad = 0,
  table_token_scan_visited = 0,
  table_token_scan_completed = 0,
  table_token_scan_terminal = 0,
  table_token_scan_transient = 0,
  table_token_scan_structural = 0,
  table_token_scan_smr_skips = 0,
  table_token_scan_payloads = 0,
  table_token_pass_epoch = 0,
  table_token_pass_desc = 0,
  table_token_pass_ack_epoch = 0,
  table_token_pass_ack_desc = 0,
  table_token_pass_act_epoch = 0,
  table_token_pass_act_control = 0,
  table_token_pass_ack_act_epoch = 0,
  table_token_pass_ack_act_control = 0,
  table_token_pass_restarts = 0,
  table_token_pass_acks = 0,
  table_token_pass_cycle = 0,
  table_token_pass_phase = 0,
  table_token_pass_lane = 0,
  table_token_pass_hazard = 0,
  table_token_pass_ack_cycle = 0,
  table_token_pass_ack_phase = 0,
  thread_scan_needscan_pending = 0,
  table_rescan_pending = 0,
  thread_scan_dirty_misses = 0,
  thread_scan_frame_fallbacks = 0,
  ffi_native_scan_attempts = 0,
  ffi_native_scan_stable_frames = 0,
  ffi_native_scan_retries = 0,
  ffi_native_scan_invalid = 0,
  sweep_owner_runs = 3963901,
  sweep_owner_arenas = 8,
  sweep_owner_live_cells = 7561,
  sweep_live_updates = 3,
  sweep_live_huge_bytes = 0,
  live_estimate = 27760,
  weak_stack = 0x7ffbecad67b0,
  weak_ready = 0x7ffbecad6bb0 "\001",
  weak_overflow = 0x0,
  weak_capacity = 128,
  weak_drain_active = 0,
  weak_write_active = 0,
  weak_mark_closed = 1,
  weak_root_scanned = 1,
  weak_count = 1,
  weak_tables_seen = 4,
  weak_tables_weakkey = 4,
  weak_tables_weakval = 4,
  weak_tables_allweak = 4,
  weak_tables_queued = 4,
  weak_tables_overflow = 0,
  weak_scan_cursor = 0,
  weak_scan_runs = 0,
  weak_scan_tables = 0,
  weak_scan_slots = 0,
  weak_scan_clearable = 0,
  weak_clear_cursor = 1,
  weak_clear_runs = 4,
  weak_clear_tables = 4,
  weak_clear_slots = 4,
  weak_clear_cleared = 0,
  weak_bridge_skipped = 4,
  weak_bridge_fallbacks = 0,
  weak_bridge_backfills = 0,
  weak_bridge_backfill_tables = 0,
  weak_bridge_backfill_slots = 0,
  weak_bridge_backfill_cleared = 0,
  finreg_cdata_sets = 0,
  finreg_cdata_clears = 0,
  finreg_cdata_queued = 0,
  finreg_cdata_sweep_queued = 0,
  finreg_cdata_pweak_queued = 0,
  finreg_cdata_preclaim_obj = 0x7ffbeca500a0,
  finreg_cdata_preclaim_fin = 0x7ffbeca300a0,
  finreg_cdata_preclaim_capacity = 4096,
  finreg_cdata_preclaim_head = 0,
  finreg_cdata_preclaim_count = 0,
  finreg_cdata_pweak_claimed = 0,
  finreg_cdata_preclaim_overflow = 0,
  finreg_cdata_preclaim_dispatched = 0,
  finreg_cdata_order_seen = 0,
  finreg_cdata_order_claimed = 0,
  finreg_cdata_order_unlinked = 0,
  finreg_cdata_order_queued = 0,
  finreg_cdata_order_retired = 0,
  finreg_cdata_order_tombstones = 0,
  finreg_cdata_order_fallbacks = 0,
  finreg_cdata_pending_order_hits = 0,
  finreg_cdata_preclaim_test_fail = 0,
  finreg_cdata_preclaim_publish_pause = 0,
  finreg_cdata_preclaim_publish_paused = 0,
  finreg_cdata_preclaim_publish_release = 0,
  finreg_udata_sets = 3,
  finreg_udata_clears = 0,
  finreg_udata_queued = 0,
  finreg_udata_head = 0x7ffbecad4a70,
  finreg_udata_retired = 0x0,
  finreg_udata_registered = 3,
  finreg_udata_retired_nodes = 0,
  finreg_udata_discovered = 0,
  finreg_udata_forgets = 0,
  finalizer_mpsc = 0x0,
  finalizer_tail = 0x0,
  finalizer_active = 0,
  finalizer_owner_actor = 0,
  finalizer_spawn_latch = 0,
  finalizer_queued = 0,
  finalizer_dequeued = 0,
  finalizer_mpsc_drained = 0,
  finalizer_enters = 12,
  finalizer_leaves = 12,
  finalizer_sweep_blocks = 0,
  finalizer_spawn_deferrals = 0,
  finalizer_spawn_release_wakes = 0,
  finalizer_drain_test_pause = 0,
  finalizer_drain_test_paused = 0,
  finalizer_drain_test_release = 0,
  weak_keys_marked = 0,
  weak_values_marked = 0,
  tg_registry_head = 0x55c6bcfa7310,
  tg_registry_nodes = 1,
  tg_registry_incomplete = 0,
  tg_registry_alloc_failures = 0,
  tg_registry_test_fail_alloc = 0,
  tg_list = 0x7ffbeccf2d40,
  n_threads = 1,
  tg_reclaiming = 0
}
$2 = {
  bump = {{
      a = 0x0,
      cell = 0,
      end = 0
    }, {
      a = 0x7ffbec9d0000,
      cell = 1772,
      end = 4096
    }},
  bins = {{0x0 <repeats 32 times>}, {0x0, 0x7ffbecadd0f0, 0x7ffbecadd2c0, 0x7ffbecadd150, 0x0, 0x7ffbecad9c50, 0x7ffbecadd250, 0x7ffbecadf470, 0x0, 0x7ffbecad9ce0, 0x0, 0x7ffbecadd190, 0x7ffbecadb110, 0x0, 0x0, 0x7ffbecad7d10, 0x0, 0x7ffbecad9db0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7ffbecad8750, 0x7ffbecad79d0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7ffbec9d66a0}},
  binmask = {0, 2172820206},
  owned = {0x0, 0x7ffbec9d0000},
  needsweep = {0x0, 0x0},
  quarantine = {0x7ffbeca90000, 0x0},
  reclaimed = {0x7ffbecab0000, 0x0},
  smalltab = 0x55c6bcfa72f0,
  sweep_epoch = 6,
  prepare_epoch = 6,
  huge_retire_cursor = 0,
  huge_reclaim_cursor = 0,
  huge_retire_done = 0 '\000',
  remote_pending = 0,
  owner_tid = 1,
  owner_tg = 0x7ffbeccf2d40,
  alloc_black = 1 '\001',
  free_noinsert = 0 '\000',
  owned_count = {0, 2},
  needsweep_count = {0, 0}
}
[Inferior 1 (process 118524) detached]
