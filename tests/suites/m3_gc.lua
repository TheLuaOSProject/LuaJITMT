local function make_clean(t)
  t:make({ "clean" }, { quiet = true, jobs = false })
end

local function make_default(t, opts)
  opts = opts or {}
  t:make(opts.args, { quiet = true, jobs = opts.jobs })
end

local function compile_luajit_fixture(t, out, cfile, opts)
  opts = opts or {}
  local libs = { "-lm", "-ldl" }
  if opts.pthread ~= false then
    libs[#libs + 1] = opts.pthread or "-pthread"
  end
  t:cc(out, { t:path("tests", cfile) }, {
    cflags = opts.cflags,
    link_luajit = true,
    libs = libs
  })
end

local function run_lua_test(t, name)
  t:run({ t:path("tools", "ci", "lua_test.sh"), name })
end

local function run_stock_tests(t, ...)
  local argv = {
    t:path("tools", "ci", "run_stock_tests.sh"),
    t:path("src", "luajit")
  }
  for i = 1, select("#", ...) do
    argv[#argv + 1] = select(i, ...)
  end
  t:run(argv)
end

local function run_case(cases, t, name)
  io.stderr:write("== " .. name .. " ==\n")
  cases[name].run(t)
  io.stderr:write("ok " .. name .. "\n")
end

return function(add)
  local cases = {}

  local function register(test)
    cases[test.name] = test
    add(test)
  end

  register({
    name = "m3_gc2_worker_scheduler",
    description = "staged GC2 parked-worker scheduler guard and fixtures",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      compile_luajit_fixture(t, t:tmp("lj_t-gc2-worker-scheduler"),
                             "t-gc2-worker-scheduler.c")
      t:run({ t:tmp("lj_t-gc2-worker-scheduler") })
      t:luajit({ "-joff", t:path("tests", "t-gc-workers.lua") })
      t:luajit({ t:path("tests", "t-gc-workers.lua") })

      print("M3 GC2 worker scheduler test passed")
    end
  })

  register({
    name = "m3_safepoint_handshake",
    description = "C-level safepoint handshake fixture",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"

      make_clean(t)
      make_default(t)
      compile_luajit_fixture(t, t:tmp("lj_t_safepoint_handshake"),
                             "t-safepoint-handshake.c", {
        cflags = pthread,
        pthread = pthread
      })
      t:run({ t:tmp("lj_t_safepoint_handshake") })

      print("M3 safepoint handshake tests passed")
    end
  })

  register({
    name = "m3_vm_safepoint",
    description = "focused x64 VM safepoint poll fixture",
    run = function(t)
      make_clean(t)
      make_default(t)
      compile_luajit_fixture(t, t:tmp("lj_t_vm_safepoint"),
                             "t-vm-safepoint.c")
      t:run({ t:tmp("lj_t_vm_safepoint") }, { timeout = "20s" })
    end
  })

  register({
    name = "m3_gc2_paranoia",
    description = "GC2 paranoia build, oracle fixtures, and stock tests",
    run = function(t)
      make_clean(t)
      make_default(t, {
        args = { "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" }
      })
      for _, name in ipairs({
        "t-gc2-paranoia",
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }) do
        local out = t:tmp("lj_" .. name .. "_paranoia")
        compile_luajit_fixture(t, out, name .. ".c", {
          cflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
        })
        t:run({ out })
      end
      run_stock_tests(t, "--quiet")

      make_clean(t)
      make_default(t, {
        args = {
          "BUILDMODE=static",
          "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -DLUAJIT_DISABLE_JIT"
        }
      })
      run_stock_tests(t, "--quiet", "-jit")
    end
  })

  register({
    name = "m3_gc2_scaffold",
    description = "focused M3 GC2 scaffold tests and dependent gates",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      for _, name in ipairs({
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }) do
        local out = t:tmp("lj_" .. name)
        compile_luajit_fixture(t, out, name .. ".c")
        t:run({ out })
      end

      run_case(cases, t, "m3_gc2_worker_scheduler")
      run_case(cases, t, "m3_safepoint_handshake")
      run_case(cases, t, "m3_vm_safepoint")
      run_case(cases, t, "m3_gc2_paranoia")
      run_lua_test(t, "m2_arena_all")

      make_clean(t)
      make_default(t, { jobs = false })
      make_clean(t)
      t:make({ "amalg" }, { quiet = true, jobs = false })

      t:run({ t:path("tools", "ci", "m0_matrix.sh") })
      print("M3 GC2 scaffold tests passed")
    end
  })
end
