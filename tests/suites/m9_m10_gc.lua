local utils = require("suite_utils")

local contains = utils.contains
local shell_quote = utils.shell_quote
local assert_no_lines = utils.assert_no_lines

local function write_bad_benchmark_csv(t, base, bad)
  local out = assert(io.open(bad, "wb"))
  local n = 0
  for line in (t:read(base) .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then
      n = n + 1
      if n == 1 then
        out:write(line, "\n")
      else
        local cols = {}
        for col in (line .. ","):gmatch("(.-),") do
          cols[#cols + 1] = col
        end
        cols[3] = ("%.2f"):format(assert(tonumber(cols[3])) * 2)
        out:write(table.concat(cols, ","), "\n")
      end
    end
  end
  out:close()
end

local function command_succeeded(cmd)
  local ok, _, code = os.execute(cmd)
  if type(ok) == "number" then return ok == 0 end
  return ok == true and (code == nil or code == 0)
end

local function run_output_contains(t, cmd, needle)
  t:run(cmd .. " | rg -F " .. shell_quote(needle) .. " >/dev/null")
end

local function run_output_contains_all(t, cmd, needles)
  for i = 1, #needles do
    cmd = cmd .. " | rg -F " .. shell_quote(needles[i])
  end
  t:run(cmd .. " >/dev/null")
end

local function build_and_run_alloc_account(t)
  local out = t:tmp("lj_t-gc2-alloc-account-m10")
  t:cc(out, { t:path("tests", "t-gc2-alloc-account.c") }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = "20s" })
end

local function run_gc_stats(t)
  t:build({ quiet = true })

  t:assert_all_any_contains({
    t:path("src", "lib_base.c"),
    t:path("src", "lj_obj.h"),
    t:path("src", "lj_safepoint.c"),
    t:path("tests", "t-gc-stats.lua"),
    t:path("aux", "bench", "bench_mt.lua"),
    t:path("plan", "13_testing_and_benchmarks.md")
  }, {
    "collectgarbage(\"stats\")",
    "static void gc_stats_push(lua_State *L)",
    "lua_createtable(L, 0, 86)",
    "static TValue *gc_stats_storetv_str(lua_State *L, GCtab *t, const char *name,",
    "static TValue *gc_stats_storetv_int(lua_State *L, GCtab *t, int32_t key,",
    "gc_stats_storetv_str(L, t, \"poll_ack_latency_buckets\", &tv)",
    "lj_gc_pubtabobj(L, t, bt)",
    "lj_gc_pubtab(L, t)",
    "cycle_starts",
    "minor_cycle_starts",
    "poll_ack_samples",
    "poll_ack_latency_max_ns",
    "poll_ack_latency_buckets",
    "LJ_GC2_HS_LATENCY_BUCKETS",
    "safepoint_note_ack_latency(global_State *g)",
    "poll_ack_p99_ns",
    "assist_runs",
    "worker_runs",
    "worker_idle_declares",
    "worker_busy_retries",
    "worker_wakes",
    "worker_parks",
    "worker_async_progress",
    "sweep_owner_runs",
    "sweep_live_updates",
    "major_root_scans",
    "minor_root_scans",
    "weak_legacy_backfills",
    "weak_keys_marked",
    "finreg_cdata_sweep_queued",
    "finreg_cdata_pweak_root_fallbacks",
    "finreg_cdata_order_fallbacks",
    "finreg_udata_registered",
    "finalizer_queued",
    "finalizer_mpsc_drained",
    "finalizer_spawn_deferrals"
  })

  assert_no_lines(t, "GC stats table fields must be CAS-published",
                  { t:path("src", "lib_base.c") }, function(line)
    return contains(line, "copyTVrel(L, lj_tab_setstr(L, t") or
           contains(line, "copyTVrel(L, lj_tab_setstr(L, bt") or
           contains(line, "copyTVrel(L, lj_tab_setint(L, t") or
           contains(line, "copyTVrel(L, lj_tab_setint(L, bt") or
           contains(line, "lj_tab_storetab(L, lj_tab_setstr(L, t,")
  end)

  t:luajit({ "-joff", t:path("tests", "t-gc-stats.lua") })
  t:luajit({ t:path("tests", "t-gc-stats.lua") })
  print("M9 GC stats guard passed")
end

local function run_bench_smoke(t)
  t:build({ quiet = true })

  for _, file in ipairs({ "bench.lua", "bench_mt.lua", "run.sh" }) do
    t:run({
      "cmp",
      "-s",
      t:path("aux", "bench", file),
      t:path("plan", "aux", "bench", file)
    })
  end

  t:assert_all_any_contains({
    t:path("src", "lib_threading.c"),
    t:path("aux", "bench", "bench_mt.lua"),
    t:path("aux", "bench", "run.sh")
  }, {
    "LJLIB_CF(threading_now)",
    "CLOCK_MONOTONIC",
    "local wall = assert(th.now, \"bench_mt.lua requires threading.now()\")",
    "local scale = tonumber(getenv(\"BENCH_SCALE\")) or 1",
    "if dt <= 0 then dt = 1e-9 end",
    "requires an even thread count >= 2",
    "BENCH_THREADS=\"1 2 4 8\"",
    "BENCH_FILTER=<substring>"
  })

  t:assert_not_contains(t:path("aux", "bench", "bench_mt.lua"),
                        "os.clock")

  t:luajit({ t:path("tests", "t-threading-api.lua") })

  local luajit = shell_quote(t:path("src", "luajit"))
  local bench_mt = shell_quote(t:path("aux", "bench", "bench_mt.lua"))
  run_output_contains_all(t,
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 1 chan_pingpong",
    { "chan_pingpong", "skipped: requires an even thread count >= 2" })
  run_output_contains_all(t,
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 2 chan_pingpong",
    { "chan_pingpong", "ops/s" })
  run_output_contains(t,
    "BENCH_SCALE=0.0001 BENCH_THREADS='1 2' BENCH_FILTER=arith-MT " ..
      shell_quote(t:path("aux", "bench", "run.sh")) .. " scaling " .. luajit,
    "GC stats:")
  print("M9 benchmark smoke guard passed")
end

local function run_bench_regression(t)
  t:build({ quiet = true })

  local base = t:path("bench", "baseline_372b369b9afd_.csv")
  local cur = t:tempname("lj-bench-current")
  local bad = t:tempname("lj-bench-bad")

  t:assert_all_any_contains({
    t:path("bench", "run_baseline.sh"),
    t:path("bench", "compare_baseline.sh"),
    t:path("aux", "bench", "bench.lua")
  }, {
    "BENCH_LUA=${BASELINE_BENCH_LUA:-\"$ROOT/aux/bench/bench.lua\"}",
    "BENCH_SCALE",
    "BENCH_GC_MODE",
    "COLUMN=${BENCH_COLUMN:-jit_ns_per_op}",
    "MAX=${BENCH_GEOMEAN_MAX:-1.10}",
    "geomean",
    "PASS: geomean",
    "FAIL: geomean"
  })

  local compare = shell_quote(t:path("bench", "compare_baseline.sh"))
  run_output_contains(t, compare .. " " .. shell_quote(base) .. " " ..
                         shell_quote(base),
                      "PASS: geomean 1.000000 <= 1.100000")

  write_bad_benchmark_csv(t, base, bad)
  local bad_cmd = compare .. " " .. shell_quote(base) .. " " ..
                    shell_quote(bad) .. " >/dev/null 2>&1"
  if command_succeeded(bad_cmd) then
    error("benchmark regression checker accepted a known bad CSV")
  end

  t:run("BENCH_SCALE=0.001 BASELINE_OUT=" .. shell_quote(cur) .. " " ..
        shell_quote(t:path("bench", "run_baseline.sh")) .. " " ..
        shell_quote(t:path("src", "luajit")) .. " >/dev/null")
  run_output_contains(t, compare .. " " .. shell_quote(cur) .. " " ..
                         shell_quote(cur),
                      "PASS: geomean 1.000000 <= 1.100000")
  t:remove(cur)
  t:remove(bad)
  print("M9 benchmark regression accounting guard passed")
end

local function run_generational(t)
  t:build({ quiet = true })

  t:assert_all_any_contains({
    t:path("src", "lua.h"),
    t:path("src", "lj_obj.h"),
    t:path("src", "lj_gc.h"),
    t:path("src", "lj_gc.c"),
    t:path("src", "lj_gc2.h"),
    t:path("src", "lj_gc2.c"),
    t:path("src", "lj_api.c"),
    t:path("src", "lj_meta.h"),
    t:path("src", "lj_meta.c"),
    t:path("src", "lj_tg.c"),
    t:path("src", "lib_base.c"),
    t:path("tests", "t-gc-generational-mode.lua"),
    t:path("tests", "t-gc-stats.lua"),
    t:path("tests", "t-gc2-alloc-account.c"),
    t:path("tests", "t-gc2-traverse.c")
  }, {
    "LUA_GCGENERATIONAL",
    "LUA_GCINCREMENTAL",
    "uint32_t generational",
    "uint32_t cycle_minor_requested",
    "uint32_t cycle_sweep_minor",
    "uint32_t minor_sweep_enabled;  /* Public gate for minor sweep identity. */",
    "uint32_t cycle_roots_minor",
    "uint32_t minor_roots_enabled;  /* Public gate for minor root selection. */",
    "uint32_t force_major",
    "uint64_t major_cycle_starts",
    "uint64_t minor_cycle_requests",
    "uint64_t minor_cycle_starts",
    "uint64_t minor_sweep_deferred",
    "uint64_t minor_sweep_arenas",
    "uint64_t minor_roots_deferred",
    "uint64_t major_root_scans",
    "uint64_t minor_root_scans",
    "uint64_t minor_survival_base_live",
    "uint64_t minor_survival_bytes",
    "uint32_t minor_survival_pct",
    "uint32_t minor_survival_threshold_pct",
    "uint64_t minor_survival_major_requests",
    "uint64_t cycle_alloc_bytes",
    "LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT",
    "la_store32_rlx(&g->gc2.generational, 0)",
    "uint64_t remembered_barriers",
    "uint64_t remembered_pushed",
    "uint64_t remembered_overflows",
    "uint64_t remembered_filtered",
    "uint64_t remembered_drained",
    "lj_gc2_set_generational(global_State *g, int enabled)",
    "la_store32_rel(&g->gc2.generational, want)",
    "tg->mark_active = la_load32_acq(&g->gc2.generational) != 0",
    "lj_gc2_force_major(global_State *g)",
    "lj_gc2_update_minor_survival_policy(global_State *g, uint64_t live)",
    "la_store64_rel(&g->gc2.cycle_alloc_bytes",
    "la_load64_acq(&g->gc2.minor_survival_base_live)",
    "la_add64_rlx(&g->gc2.minor_survival_major_requests",
    "if (roots_minor)",
    "gc2_update_public_minor_gates(global_State *g)",
    "la_store32_rel(&g->gc2.minor_sweep_enabled, enabled)",
    "la_store32_rel(&g->gc2.minor_roots_enabled, enabled)",
    "gc2_remember_obj(global_State *g, GCobj *o)",
    "gc2_remember_pair(global_State *g, GCobj *parent, GCobj *child)",
    "lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child)",
    "gc2_flush_and_drain_ssb(global_State *g)",
    "lj_meta_tsettv_pair(lua_State *L, cTValue *o, cTValue *k",
    "lj_gc2_scan_minor_roots(global_State *g, lua_State *L)",
    "lj_gc2_scan_cycle_roots(global_State *g, lua_State *L)",
    "gc2_scan_pending_roots(global_State *g)",
    "if (!sweep_minor)",
    "LJ_GC2_HS_ALLOC_WHITE : LJ_GC2_HS_ALLOC_BLACK",
    "lj_gc_sweep_gc2_unmarked(global_State *g)",
    "gc_chain_splice(p, o)",
    "gc2_unlink_root_obj(g, o);",
    "tg->alloc.alloc_black =",
    "la_load32_acq(&g->gc2.cycle_sweep_minor) == 0",
    "la_add64_rlx(&g->gc2.major_root_scans",
    "la_add64_rlx(&g->gc2.minor_root_scans",
    "lj_gc2_finreg_cdata_preclaim(L, g, obj2gco(preclaim_cd)",
    "old_survivor",
    "lj_gc2_force_major(g);  /* First generational cycle establishes old marks. */",
    "lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent",
    "lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent",
    "test_vm_generational_table_store_remembered",
    "test_jit_generational_table_store_remembered",
    "active_ssb_last(tg) == obj2gco(parent)",
    "gc_stats_setint(L, t, \"generational\"",
    "gc_stats_setint(L, t, \"cycle_sweep_minor\"",
    "gc_stats_setint(L, t, \"cycle_roots_minor\"",
    "minor_cycle_requests",
    "minor_cycle_starts",
    "minor_sweep_deferred",
    "minor_roots_deferred",
    "major_root_scans",
    "minor_root_scans",
    "minor_survival_pct",
    "minor_survival_major_requests",
    "cycle_alloc_bytes",
    "remembered_barriers",
    "remembered_filtered",
    "remembered_drained",
    "collectgarbage(\"generational\")",
    "collectgarbage(\"incremental\")"
  })

  t:assert_text_ordered("lj_gc_sweep_gc2_unmarked",
    t:c_block(t:path("src", "lj_gc.c"),
              "uint32_t lj_gc_sweep_gc2_unmarked"), {
      "gc_chain_splice(p, o)",
      "gc2_free_unmarked_obj(g, o)"
    })
  t:assert_text_ordered("gc2_sweep_arena_bodies",
    t:c_block(t:path("src", "lj_gc.c"),
              "static uint32_t gc2_sweep_arena_bodies"), {
      "gc2_unlink_root_obj(g, o)",
      "gc2_free_unmarked_obj(g, o)"
    })

  t:luajit({ "-joff", t:path("tests", "t-gc-generational-mode.lua") })
  t:luajit({ t:path("tests", "t-gc-generational-mode.lua") })
  build_and_run_alloc_account(t)
  print("M10 generational mode guard passed")
end

return function(add)
  add({
    name = "m9_gc_stats",
    description = "GC stats telemetry table and smoke test",
    run = run_gc_stats
  })

  add({
    name = "m9_bench_smoke",
    description = "benchmark harness smoke and drift guard",
    run = run_bench_smoke
  })

  add({
    name = "m9_bench_regression",
    description = "benchmark CSV/geomean accounting guard",
    run = run_bench_regression
  })

  add({
    name = "m10_generational",
    description = "public generational GC mode and accounting guard",
    run = run_generational
  })

  add({
    name = "m9_m10_gc",
    description = "M9/M10 aggregate telemetry and generational gates",
    run = function(t)
      run_gc_stats(t)
      run_bench_smoke(t)
      run_bench_regression(t)
      run_generational(t)
      print("M9/M10 GC gates passed")
    end
  })
end
