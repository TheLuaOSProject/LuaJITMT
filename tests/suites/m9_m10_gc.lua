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
