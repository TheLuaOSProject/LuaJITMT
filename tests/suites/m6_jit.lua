local utils = require("suite_utils")

local contains = utils.contains
local shell_quote = utils.shell_quote
local count_plain = utils.count_plain
local lines = utils.iter_lines
local assert_no_lines = utils.assert_no_lines

local function count_match(s, pattern)
  local count = 0
  for _ in s:gmatch(pattern) do count = count + 1 end
  return count
end

local function source_code_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h", ".dasc" } })
end

local function source_and_test_files(t)
  local files = source_code_files(t)
  local tests = t:files(t:path("tests"), {
    extensions = { ".c", ".h", ".lua" }
  })
  for i = 1, #tests do files[#files + 1] = tests[i] end
  table.sort(files)
  return files
end

local function lua_path(t)
  return t:path("src", "?.lua") .. ";" .. t:path("src", "jit", "?.lua") .. ";;"
end

local function luajit_code(t, code, opts)
  opts = opts or {}
  t:luajit({ "-e", code }, {
    env = { LUA_PATH = lua_path(t) },
    timeout = opts.timeout
  })
end

local function luajit_file(t, file, opts)
  opts = opts or {}
  t:luajit({ file }, {
    env = opts.lua_path and { LUA_PATH = lua_path(t) } or nil,
    timeout = opts.timeout
  })
end

local function luajit_dump(t, dump, dumpopt, code, opts)
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(lua_path(t)) }
  if opts.timeout then parts[#parts + 1] = "timeout " .. shell_quote(opts.timeout) end
  parts[#parts + 1] = shell_quote(t:path("src", "luajit"))
  parts[#parts + 1] = shell_quote(dumpopt)
  parts[#parts + 1] = "-e " .. shell_quote(code)
  t:run(table.concat(parts, " ") .. " >" .. shell_quote(dump) .. " 2>&1",
        { quiet = opts.quiet })
end

local function build_default(t)
  t:make(nil, { quiet = true, jobs = false })
end

local function build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  if opts.build ~= false then
    if opts.clean then
      t:build({ clean = true, quiet = true })
    else
      build_default(t)
    end
  end
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = opts.timeout })
end

local function assert_dump_contains(t, dump, needle, label)
  local data = t:read(dump)
  if not contains(data, needle) then
    error(label .. ": missing dump text: " .. needle, 2)
  end
end

local function assert_dump_match(t, dump, pattern, label)
  local data = t:read(dump)
  if not data:match(pattern) then
    error(label .. ": missing dump pattern: " .. pattern, 2)
  end
end

local function assert_dump_all_contains(t, dump, needles, label)
  for i = 1, #needles do
    assert_dump_contains(t, dump, needles[i], label)
  end
end

local function trace1_ir_state(t, dump)
  local st = {
    array = 0,
    hdradd = 0,
    xload = 0,
    asize = 0,
    eq = 0,
    aref = false,
    aload = false,
    xpoll = false,
    ule = false,
    href = false,
    hrefk = false,
    hmask = false,
    node = false,
    done = false
  }
  local inir = false
  for line in lines(t:read(dump)) do
    if contains(line, "---- TRACE 1 IR") then
      inir = true
    elseif inir and contains(line, "---- TRACE 1 stop") then
      st.done = true
      break
    elseif inir then
      if line:match("FLOAD .*tab[.]array") then st.array = st.array + 1 end
      if contains(line, " p64 ADD ") and contains(line, "-16") then
        st.hdradd = st.hdradd + 1
      end
      if contains(line, " XLOAD ") then st.xload = st.xload + 1 end
      if line:match("FLOAD .*tab[.]asize") then st.asize = st.asize + 1 end
      if contains(line, " p64 EQ ") then st.eq = st.eq + 1 end
      if contains(line, " ULE ") then st.ule = true end
      if contains(line, " HREF ") then st.href = true end
      if contains(line, " HREFK") then st.hrefk = true end
      if contains(line, " AREF ") then st.aref = true end
      if contains(line, " ALOAD ") then st.aload = true end
      if contains(line, " XPOLL ") or contains(line, "XPOLL") then st.xpoll = true end
      if contains(line, "tab.hmask") then st.hmask = true end
      if contains(line, "tab.node") then st.node = true end
    end
  end
  return st
end

local function assert_trace1_ir(t, dump, label, pred)
  local st = trace1_ir_state(t, dump)
  if not st.done or not pred(st) then
    io.stderr:write(t:read(dump))
    error(label, 2)
  end
end

local function assert_marker_set(t, paths, needles, label)
  for i = 1, #needles do
    local ok = false
    for j = 1, #paths do
      if contains(t:read(paths[j]), needles[i]) then
        ok = true
        break
      end
    end
    if not ok then error(label .. ": missing marker: " .. needles[i], 2) end
  end
end

local function assert_section_no_lines(t, label, path, start_text, end_text, pred)
  local data = t:text_between(path, start_text, end_text)
  local hits = {}
  local n = 0
  for line in lines(data) do
    n = n + 1
    if pred(line) then hits[#hits + 1] = tostring(n) .. ": " .. line end
  end
  if #hits > 0 then error(label .. ":\n" .. table.concat(hits, "\n"), 2) end
end

local function assert_block_no_lines(t, label, path, start_text, pred)
  local data = t:c_block(path, start_text)
  local hits = {}
  local n = 0
  for line in lines(data) do
    n = n + 1
    if pred(line) then hits[#hits + 1] = tostring(n) .. ": " .. line end
  end
  if #hits > 0 then error(label .. ":\n" .. table.concat(hits, "\n"), 2) end
end

local function assert_order_positions(label, data, ordered)
  local pos = 1
  local found = {}
  for i = 1, #ordered do
    local p = data:find(ordered[i], 1, true)
    if not p then error(label .. ": missing expected text: " .. ordered[i], 2) end
    found[i] = p
  end
  for i = 2, #found do
    if found[i - 1] >= found[i] then
      error(label .. ": wrong ordering near: " .. ordered[i], 2)
    end
  end
  pos = pos
end

local function raw_index_write(line, name_pattern)
  return line:match(name_pattern .. "%[[^%]]+%]%s*=[^=]") ~= nil
end

local function raw_cast_write(line)
  if contains(line, "lj_mcode_rw") then return false end
  local eq = line:find("=", 1, true)
  if not eq or line:sub(eq + 1, eq + 1) == "=" then return false end
  local lhs = line:sub(1, eq - 1)
  return contains(lhs, "*(uint16_t *)") or
         contains(lhs, "*(uint32_t *)") or
         contains(lhs, "*(uint64_t *)") or
         contains(lhs, "*(int16_t *)") or
         contains(lhs, "*(int32_t *)") or
         contains(lhs, "*(int64_t *)")
end

local function x64_cmp_poll_pattern()
  return "cmp dword %[r14%+0x[0-9a-f]+%], %+0x00"
end

local function assert_loop_ir_markers(t, dump, label, markers)
  local data = t:read(dump)
  local loop = false
  local seen = {}
  for line in lines(data) do
    if contains(line, "------ LOOP") then
      loop = true
    elseif contains(line, "---- TRACE 1 mcode") then
      break
    elseif loop then
      for i = 1, #markers do
        if contains(line, markers[i]) then seen[markers[i]] = true end
      end
    end
  end
  for i = 1, #markers do
    if not seen[markers[i]] then
      io.stderr:write(data)
      error(label .. ": missing loop marker: " .. markers[i], 2)
    end
  end
end

local function assert_call_after_loop_polls(t, dump, label, call, mincmp, extra)
  local data = t:read(dump)
  local loop, cmp, done = false, 0, false
  local state = {}
  for line in lines(data) do
    if contains(line, "->LOOP:") then loop = true end
    if loop and line:match(x64_cmp_poll_pattern()) then cmp = cmp + 1 end
    if loop and extra then extra(line, state) end
    if loop and contains(line, call) then
      done = true
      break
    end
  end
  if not done or cmp < mincmp or (extra and state.fail) then
    io.stderr:write(data)
    error(label, 2)
  end
  return state
end

local m6_cases = {
  "m6_dispatch_redispatch",
  "m6_jit_token",
  "m6_jit_cell_ops",
  "m6_jit_barrier_xpoll",
  "m6_jit_xbar_xpoll",
  "m6_jit_table_store_helper",
  "m6_jit_aref_pair_guard",
  "m6_jit_hrefk_nodehdr",
  "m6_jit_href_nodehdr",
  "m6_jit_alloc_account",
  "m6_jit_gc2_readiness",
  "m6_jit_gcstep_guard",
  "m6_jit_mcode_publish",
  "m6_jit_flush_hs"
}

local function check_m6_aggregate(t)
  t:assert_contains(t:path("tools", "ci", "m6_jit.sh"), "m6_jit")
end

local function table_store_smoke()
  return [=[
local util = require("jit.util")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(util.traceinfo(1), "shared existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hhole = { stable = 0 }
hhole.stable = nil
for i = 1, 200 do
  hhole.stable = i
  hhole.stable = nil
end
assert(hhole.stable == nil)
assert(util.traceinfo(1), "previous-nil hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(util.traceinfo(1), "shared existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local ahole = { 0, nil, 0 }
for i = 1, 200 do
  ahole[2] = i
  ahole[2] = nil
end
assert(ahole[2] == nil)
assert(util.traceinfo(1), "previous-nil array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wk = setmetatable({ stable = 0 }, { __mode = "k" })
for i = 1, 200 do
  wk.stable = i
end
assert(wk.stable == 200)
assert(util.traceinfo(1), "weak-key existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wv = setmetatable({ 0 }, { __mode = "v" })
for i = 1, 200 do
  wv[1] = i
end
assert(wv[1] == 200)
assert(util.traceinfo(1), "weak-value existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function hash_insert(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local hi = hash_insert(80)
assert(hi.stable == 80)
assert(util.traceinfo(1), "trace-local hash insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local ai = array_insert(80)
assert(ai[1] == 80)
assert(util.traceinfo(1), "trace-local array insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function phi_store(n)
  local a = { stable = 0 }
  local b = { stable = 0 }
  local t = a
  for i = 1, n do
    if i == 1 then t = a else t = b end
    t.stable = i
  end
  return a.stable + b.stable
end
assert(phi_store(80) == 81)
assert(util.traceinfo(1), "PHI-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local up = { stable = 0 }
local function upvalue_store(n)
  for i = 1, n do
    up.stable = i
  end
  return up.stable
end
assert(upvalue_store(80) == 80)
assert(util.traceinfo(1), "upvalue-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_escaped_store()
  local sink
  return function(n)
    for i = 1, n do
      local t = { stable = 0 }
      sink = t
      t.stable = i
    end
    return sink.stable
  end
end
local escaped_store = make_escaped_store()
assert(escaped_store(80) == 80)
assert(util.traceinfo(1), "closed-upvalue escaped existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_nested_escape()
  local sink
  return function(n)
    for i = 1, n do
      local outer = { inner = false }
      local t = { stable = 0 }
      outer.inner = t
      sink = outer
      t.stable = i
    end
    return sink.inner.stable
  end
end
local nested_escape = make_nested_escape()
assert(nested_escape(80) == 80)
assert(util.traceinfo(1), "nested escaped existing table store did not trace")
]=]
end

return function(add)
  add({
    name = "m6_dispatch_redispatch",
    description = "M6 dispatch redispatch and x64 TG-local dispatch guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t_safepoint_handshake"),
                      "t-safepoint-handshake.c",
                      { build = false })

      assert_marker_set(t, {
        t:path("src", "lj_dispatch.c"),
        t:path("src", "lj_safepoint.c")
      }, {
        "uint32_t redispatch = 0",
        "la_load32_acq(&g->gc2.n_threads) > 1",
        "lj_tg_sync_dispatch(g)",
        "lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH)",
        "lj_tg_sync_dispatch_tg(g, tg)"
      }, "dispatch redispatch")

      assert_marker_set(t, {
        t:path("src", "vm_x64.dasc"),
        t:path("src", "lj_dispatch.c")
      }, {
        "load_G TMPR",
        "mov dword GL:TMPR->vmstate",
        "load_J CARG1",
        "load_DISPATCH RB",
        "TG_OFS_DISPATCH",
        "TGPOLL, dword [DISPATCH+DISPATCH_TG(poll)]",
        "static void dispatch_setrecord",
        "rec_owner = lj_trace_state_load(J) != LJ_TRACE_IDLE",
        "dispatch_setrecord(tg->dispatch, mode)"
      }, "x64 dispatch localization")

      local vm = t:path("src", "vm_x64.dasc")
      t:assert_not_contains(vm, "Secondary TGs interpret until RID_DISPATCH is local")
      assert_no_lines(t, "x64 VM must not derive g/J from fixed DISPATCH offsets",
                      { vm }, function(line)
        return line:match("DISPATCH_[GJ]%(") ~= nil
      end)
      assert_no_lines(t, "x64 VM must not derive g/J from fixed TG dispatch offsets",
                      { vm }, function(line)
        return contains(line, "TG_DISP2J") or contains(line, "TG_DISP2G")
      end)
      assert_no_lines(t, "x64 VM entry must use the running TG dispatch table",
                      { vm }, function(line)
        return contains(line, "GG_G2TGDISP") or
               (contains(line, "L:RB->glref") and contains(line, "dispatch")) or
               contains(line, "add DISPATCH, GG_G2TGDISP")
      end)
      assert_no_lines(t, "transitional TG dispatch offset macros must stay removed",
                      { t:path("src", "lj_dispatch.h") }, function(line)
        return contains(line, "GG_OFS_TGDISP") or contains(line, "GG_G2TGDISP") or
               contains(line, "TG_DISP2J") or contains(line, "TG_DISP2G")
      end)
      t:assert_not_contains(t:path("src", "lj_dispatch.c"), "DISPMODE_REC")
      check_m6_aggregate(t, "m6_dispatch_redispatch.sh")
      print("M6 dispatch redispatch guard passed")
    end
  })

  add({
    name = "m6_jit_token",
    description = "M6 JIT recorder token and x64 XPOLL scaffold guards",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_trace.h"),
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_dispatch.c"),
        t:path("src", "lj_snap.h"),
        t:path("src", "lj_snap.c"),
        t:path("src", "lj_tg.h"),
        t:path("src", "lj_err.c"),
        t:path("src", "vm_x64.dasc"),
        t:path("src", "lj_ir.h"),
        t:path("src", "lj_opt_loop.c"),
        t:path("src", "lj_asm.c"),
        t:path("src", "lj_emit_x86.h"),
        t:path("src", "lj_asm_x86.h"),
        t:path("src", "lj_record.c")
      }, {
        "uint32_t jit_token",
        "lj_jit_token_try(jit_State *J)",
        "emit_leatg(as, dest, tmptv)",
        "DISPATCH_TG(jit_base)",
        "emit_gettg(as, tmp, gl)",
        "XPOLL",
        "emitir_raw(IRTG(IR_XPOLL, IRT_NIL), 0, 0)",
        "LJ_TRACE_FUNCF_XPOLL_DEPTH",
        "static void rec_func_xpoll(jit_State *J)",
        "rec_func_xpoll(J)",
        "case IR_XPOLL: asm_xpoll(as); break;",
        "static void asm_xpoll(ASMState *as)",
        "emit_gmroi(as, XG_ARITHi(XOg_CMP), RID_DISPATCH, DISPATCH_TG(poll), 0)",
        "static int trace_poll_pending(lua_State *L)",
        "!trace_poll_pending(L)",
        "static void emit_pushx",
        "static void emit_popx",
        "static int asm_fuseggfref",
        "static int asm_x86_isvmstate",
        "la_cas32(&g->jit_token, &expect, tg->tid, LA_ACQ_REL, LA_ACQ)",
        "void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)",
        "lj_snap_restore_exit(jit_State *J, void *exptr, lua_State *L,",
        "int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,",
        "int jit_exitcode",
        "G2TG(g)->jit_exitcode",
        "tg->jit_exitcode",
        "J->L = L;",
        "lj_jit_token_held(J)",
        "lj_jit_token_release(J)",
        "lj_trace_state_load(jit_State *J)",
        "lj_trace_state_store(jit_State *J, TraceState st)",
        "lj_trace_state_store_active(jit_State *J,",
        "void lj_trace_abort(global_State *g)",
        "la_cas32((uint32_t *)&J->state, &old, next,"
      }, "recorder token")

      assert_no_lines(t, "recorder token must never block or spin-wait",
                      source_code_files(t), function(line)
        return line:match("while .*jit_token") ~= nil or
               contains(line, "la_futex_wait(&g->jit_token") or
               line:match("la_futex_wait%([^%)]*jit_token") ~= nil
      end)

      local trace_h = t:path("src", "lj_trace.h")
      local trace_c = t:path("src", "lj_trace.c")
      assert_no_lines(t, "jit_State.state must use trace state helpers",
                      source_and_test_files(t), function(line, path)
        if path == t:path("tests", "suites", "m6_jit.lua") then return false end
        if not (contains(line, "J->state") or line:match("G2J%([^%)]*%)->state")) then
          return false
        end
        if (path == trace_h or path == trace_c) and
           (contains(line, "la_load32_acq((uint32_t *)&J->state)") or
            contains(line, "la_store32_rel((uint32_t *)&J->state,") or
            contains(line, "la_cas32((uint32_t *)&J->state,")) then
          return false
        end
        return true
      end)

      assert_no_lines(t, "secondary TGs must be allowed to record and enter x64 traces",
                      { t:path("src", "lj_trace.c"), t:path("src", "vm_x64.dasc") },
                      function(line)
        return contains(line, "tg != g->main_tg") or
               contains(line, "Temporary until x64 RID_DISPATCH addressing is localized") or
               contains(line, "Secondary TGs interpret until RID_DISPATCH is local")
      end)
      assert_no_lines(t, "fixed TG fields must use DISPATCH_TG symbolic offsets",
                      { t:path("src", "lj_asm_x86.h"), t:path("src", "lj_emit_x86.h") },
                      function(line)
        return line:match("dispofs%(as, &J2TG%(as%->J%)->(jit_base)") or
               line:match("dispofs%(as, &J2TG%(as%->J%)->(tmptv)") or
               line:match("dispofs%(as, &J2TG%(as%->J%)->(cur_L)") or
               line:match("dispofs%(as, &J2TG%(as%->J%)->(gl)")
      end)
      assert_no_lines(t, "generic x64 emitter must not use recorder-TG dispatch offsets",
                      { t:path("src", "lj_asm_x86.h"), t:path("src", "lj_emit_x86.h") },
                      function(line)
        return contains(line, "dispofs(") or contains(line, "J2TG(as->J)->dispatch") or
               contains(line, "uint64_t dispaddr") or contains(line, "GG_OFS_TGDISP")
      end)
      check_m6_aggregate(t, "m6_jit_token.sh")
      t:assert_not_contains(t:path("src", "lj_trace.c"), "++snap->count")

      do
        local vm = t:read(t:path("src", "vm_x64.dasc"))
        local hotloop, stitch, bad_l, bad_exit, exitpath, winonly = false, false, false, false, false, false
        for line in lines(vm) do
          if contains(line, "->vm_hotloop:") then hotloop = true end
          if contains(line, "->vm_callhook:") then hotloop = false end
          if contains(line, "Stitch a new trace to the previous trace") then stitch = true end
          if contains(line, "call extern lj_dispatch_stitch") then stitch = false end
          if (hotloop or stitch) and contains(line, "DISPATCH_J(L)") then bad_l = true end
          if stitch and contains(line, "DISPATCH_J(exitno)") then bad_exit = true end

          if contains(line, "->vm_exit_handler:") then exitpath = true end
          if contains(line, "->vm_exit_interp:") then exitpath = false end
          if exitpath and contains(line, "|.if X64WIN") then winonly = true end
          if winonly and (contains(line, "|.else") or contains(line, "|.endif")) then
            winonly = false
          end
          if exitpath and not winonly and
             (contains(line, "DISPATCH_J(L)") or contains(line, "DISPATCH_J(parent)") or
              contains(line, "DISPATCH_J(exitno)")) then
            error("x64/POSIX trace exit restore state must stay call-local before side-token acquisition", 2)
          end
        end
        if bad_l then
          error("x64 hotloop/stitch must not write J->L before token acquisition", 2)
        end
        if bad_exit then
          error("x64 stitch must not write J->exitno before token acquisition", 2)
        end
      end

      build_and_run_c(t, t:tmp("lj_t-jit-token"), "t-jit-token.c",
                      { build = false, timeout = "20s" })
      luajit_file(t, t:path("tests", "t-jit-secondary.lua"), { timeout = "20s" })

      local dump = t:tmp("lj_t-jit-xpoll.dump")
      luajit_dump(t, dump, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
local s=0.0
for i=1,64 do s=s+i end
assert(s==2080.0)
]=], { timeout = "20s" })
      assert_dump_contains(t, dump, "XPOLL", "x64 loop trace")
      local d = t:read(dump)
      if not contains(d, "->LOOP:") or not d:match(x64_cmp_poll_pattern()) then
        error("x64 IR_XPOLL must lower to a TG poll at the loop label", 2)
      end

      local funcf_dump = t:tmp("lj_t-jit-xpoll-funcf.dump")
      luajit_dump(t, funcf_dump, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1","callunroll=32","recunroll=32")
local function f10(x) return x+1 end
local function f9(x) return f10(x)+1 end
local function f8(x) return f9(x)+1 end
local function f7(x) return f8(x)+1 end
local function f6(x) return f7(x)+1 end
local function f5(x) return f6(x)+1 end
local function f4(x) return f5(x)+1 end
local function f3(x) return f4(x)+1 end
local function f2(x) return f3(x)+1 end
local function f1(x) return f2(x)+1 end
local s=0
for i=1,64 do s=s+f1(i) end
assert(s==2720)
]=], { timeout = "20s" })
      d = t:read(funcf_dump)
      if count_plain(d, "XPOLL") < 4 then
        error("deep inlined FUNCF traces must materialize depth XPOLL", 2)
      end
      if count_match(d, x64_cmp_poll_pattern()) < 4 then
        error("FUNCF-depth IR_XPOLL must lower to TG poll checks", 2)
      end
      print("M6 JIT recorder token guard passed")
    end
  })

  add({
    name = "m6_jit_cell_ops",
    description = "M6 local-cell JIT recording behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj_m6_jit_cell_ops.dump")
      luajit_dump(t, dump, "-jdump=i", [=[
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local v, f = run(200)
assert(v == 200 and f() == 200)
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner numeric trace")
      assert_dump_contains(t, dump, "UREFC", "owner numeric UREFC")
      assert_dump_contains(t, dump, "ULOAD", "owner numeric ULOAD")
      assert_dump_contains(t, dump, "USTORE", "owner numeric USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local pool = { "even", "odd" }
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = pool[1]
  local function get() return x end
  for i = 1, n do x = pool[(i % 2) + 1] end
  return get()
end
assert(run(200) == pool[1])
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner GC-valued trace")
      assert_dump_contains(t, dump, "UREFC", "owner GC-valued UREFC")
      assert_dump_contains(t, dump, "USTORE", "owner GC-valued USTORE")
      assert_dump_contains(t, dump, "OBAR", "owner GC-valued OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(200)
assert(v == 200 and f() == 200)
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded v4 CGET/CSET trace")
      assert_dump_contains(t, dump, "UREFC", "loaded v4 CGET/CSET UREFC")
      assert_dump_contains(t, dump, "ULOAD", "loaded v4 CGET/CSET ULOAD")
      assert_dump_contains(t, dump, "USTORE", "loaded v4 CGET/CSET USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
local function run(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local f = run(30)
assert(f() == f)
assert(util.traceinfo(1), "source CNEW/FNEW creation should trace")
]=])
      assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "source CNEW helper call")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source FNEW helper call")
      assert_dump_contains(t, dump, "UREFC", "source CNEW/FNEW UREFC")
      assert_dump_contains(t, dump, "USTORE", "source CNEW/FNEW USTORE")
      assert_dump_contains(t, dump, "OBAR", "source CNEW/FNEW OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
local src = function(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
assert(f() == f)
assert(util.traceinfo(1), "loaded CNEW/FNEW creation should trace")
]=])
      assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "loaded CNEW helper call")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded FNEW helper call")
      assert_dump_contains(t, dump, "UREFC", "loaded CNEW/FNEW UREFC")
      assert_dump_contains(t, dump, "USTORE", "loaded CNEW/FNEW USTORE")
      assert_dump_contains(t, dump, "OBAR", "loaded CNEW/FNEW OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "source mixed raw-local CNEW/FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source mixed raw-local FNEW trace")
      assert_dump_contains(t, dump, "TMPREF", "source mixed raw-local TMPREF")
      assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "source mixed raw-local sync helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source mixed raw-local FNEW helper")
      assert_dump_contains(t, dump, "UREFC", "source mixed raw-local UREFC")
      assert_dump_contains(t, dump, "USTORE", "source mixed raw-local USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "loaded mixed raw-local CNEW/FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded mixed raw-local FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "loaded mixed raw-local sync helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded mixed raw-local FNEW helper")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local dummy = function() end
  local x = 0
  local keep = dummy
  for i = 1, n do
    x = x + 1
    if i >= 2 then
      local function f() return f, x end
      keep = f
    end
  end
  return keep, x
end
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "source first-promotion FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source first-promotion FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "source first-promotion helper")
      assert_dump_contains(t, dump, "NULL", "source first-promotion stack snapshot argument")
      assert_dump_match(t, dump, "SLOAD.*I", "source first-promotion inherited cell reload")
      assert_dump_contains(t, dump, "UREFC", "source first-promotion UREFC")
      assert_dump_contains(t, dump, "USTORE", "source first-promotion USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local dummy = function() end
  local x = 0
  local keep = dummy
  for i = 1, n do
    x = x + 1
    if i >= 2 then
      local function f() return f, x end
      keep = f
    end
  end
  return keep, x
end
local run = assert(loadstring(string.dump(src)))
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "loaded first-promotion FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded first-promotion FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "loaded first-promotion helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded first-promotion FNEW helper")

      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    x = x + 1
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 30)
assert(util.traceinfo(1), "pre-FNEW promoted local update should trace")
]=])
      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    local function f() return f, x end
    x = x + 1
    keep = f
  end
  return keep, x
end
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "post-FNEW promoted local update should trace")
]=])
      print("M6 JIT local-cell behavior passed")
    end
  })

  add({
    name = "m6_jit_barrier_xpoll",
    description = "x64 trace barrier behavior across XPOLL poll regions",
    run = function(t)
      build_default(t)
      check_m6_aggregate(t, "m6_jit_barrier_xpoll.sh")

      local tbar = t:tmp("lj_t-jit-tbar-xpoll.dump")
      luajit_dump(t, tbar, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local t={}
local mts={}
for i=1,80 do mts[i]={} end
jit.on()
for i=1,64 do setmetatable(t, mts[i]) end
]=], { timeout = "20s" })
      assert_loop_ir_markers(t, tbar, "setmetatable loop", { "XPOLL", "FSTORE", "TBAR" })
      assert_call_after_loop_polls(t, tbar,
                                   "post-XPOLL TBAR must lower to poll+mark checks and GC2 call",
                                   "lj_gc2_barrier_tab_g", 2)

      local obar = t:tmp("lj_t-jit-obar-xpoll.dump")
      luajit_dump(t, obar, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local uv
local vals={}
for i=1,80 do vals[i]={} end
jit.on()
local function f()
  for i=1,64 do uv=vals[i] end
end
f()
assert(uv==vals[64])
]=], { timeout = "20s" })
      assert_loop_ir_markers(t, obar, "upvalue loop", { "XPOLL", "USTORE", "OBAR" })
      local data = t:read(obar)
      local loop, test, cmp, pubuv, store_before = false, 0, 0, false, false
      for line in lines(data) do
        if contains(line, "->LOOP:") then loop = true end
        if loop and line:match("test byte") then test = test + 1 end
        if loop and line:match(x64_cmp_poll_pattern()) then cmp = cmp + 1 end
        if loop and contains(line, "lj_func_storeuv_forjit") and not pubuv then
          store_before = true
        end
        if loop and contains(line, "lj_gc_pubuv") then
          pubuv = true
          break
        end
      end
      if not pubuv or test < 2 or cmp < 2 then
        error("post-XPOLL OBAR must lower to legacy tests, poll+mark checks and pubuv call", 2)
      end
      if not store_before then
        error("x64 upvalue USTORE must release-copy before OBAR publication", 2)
      end
      print("M6 JIT XPOLL barrier behavior passed")
    end
  })

  add({
    name = "m6_jit_xbar_xpoll",
    description = "FFI XBAR aliasing respects XPOLL poll regions",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_opt_mem.c"),
        t:path("src", "lj_asm.c"),
        t:path("src", "lj_opt_fold.c")
      }, {
        "static LJ_AINLINE IRRef poll_alias_limit(jit_State *J, IRRef lim)",
        "J->chain[IR_XBAR] > lim",
        "J->chain[IR_XPOLL] > lim",
        "lim = poll_alias_limit(J, lim);",
        "case IR_NOP: case IR_XBAR:",
        "LJFOLD(XBAR)"
      }, "XBAR/XPOLL alias")
      if count_plain(t:read(t:path("src", "lj_opt_mem.c")),
                     "lim = poll_alias_limit(J, lim);") < 2 then
        error("XLOAD forwarding and XSTORE DSE must both honor XPOLL", 2)
      end
      check_m6_aggregate(t, "m6_jit_xbar_xpoll.sh")
      t:run({ t:path("tools", "ci", "m5_jit_hash_store_nyi.sh") })
      print("M6 JIT XBAR/XPOLL alias guard passed")
    end
  })

  add({
    name = "m6_jit_table_store_helper",
    description = "M6 helper-backed table store bridge guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      assert_marker_set(t, {
        t:path("src", "lj_tab.c"),
        t:path("src", "lj_tab.h"),
        t:path("src", "lj_ircall.h"),
        t:path("src", "lj_asm_x86.h"),
        t:path("src", "lj_record.c"),
        t:path("src", "lj_opt_mem.c"),
        t:path("tests", "t-jit-forward-store.c")
      }, {
        "tab_ptr_index(uintptr_t base, uintptr_t elem,",
        "tab_forwarded_jit_array_slot(lua_State *L, GCtab *parent",
        "tab_forwarded_jit_hash_slot(GCtab *parent, TValue *dst,",
        "tab_current_jit_array_slot(lua_State *L, GCtab *parent",
        "tab_current_jit_hash_slot(lua_State *L, GCtab *parent",
        "dst = tab_current_jit_array_slot(L, parent, orig, key);",
        "dst = tab_current_jit_hash_slot(L, parent, orig, key, &keycopy,",
        "return lj_tab_setint(L, parent, (int32_t)key);",
        "lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK",
        "JIT array store saw FORWARD after key/current routing.",
        "JIT hash store saw FORWARD after key/current routing.",
        "lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent",
        "lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent",
        "lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent",
        "cTValue *key)",
        "dst = lj_tab_set(L, parent, key);",
        "JIT NEWREF store saw FORWARD after key resolve.",
        "lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */",
        "lj_gc2_barrier_weak_write(L, parent, NULL, dst);  /* M8: traced weak-value array write. */",
        "lj_gc2_barrier_weak_write(L, parent, barrier_key, dst);  /* M8: traced weak hash write. */",
        "lj_gc2_barrier_weak_write(L, parent, key, dst);  /* M8: traced NEWREF weak write. */",
        "IRCALL_lj_tab_storetv_forjit_array",
        "IRCALL_lj_tab_storetv_forjit_hash",
        "IRCALL_lj_tab_storetv_forjit_newref",
        "tabref = IR(xref->op1)->op1",
        "xref->o == IR_NEWREF",
        "id = IRCALL_lj_tab_storetv_forjit_newref",
        "int keyistv = 1;",
        "args[4] = keyistv ? ASMREF_TMP2 : keyref;  /* cTValue *key or MSize index */",
        "IRTMPREF_IN2",
        "emit_leatg(as, dest, tmptv2);",
        "IRTMPREF_IN1|IRTMPREF_IN2",
        "asm_ahstore_forjit(ASMState *as, IRIns *ir)",
        "#if defined(__linux__) && LJ_TARGET_X64",
        "IRRef lim = poll_alias_limit(J, xref);",
        "M6: numeric NEWREF/HSTORE uses the generic returned-slot helper.",
        "M6: previous-nil in-bounds ASTORE/HSTORE uses the helper bridge.",
        "lj_tab_storetv_forjit_array(L, t, &oldarray[key], &src, (MSize)key);",
        "lj_tab_storetv_forjit_hash(L, t, &oldn->val, &src, &keytv);",
        "exercise_array_retiring_jit(L)",
        "exercise_hash_retiring_jit(L)",
        "lj_tab_storetv_forjit_newref(L, t, &oldarray[key], &src, &keytv);",
        "lj_tab_storetv_forjit_newref(L, t, &oldn->val, &src, &keytv);",
        "exercise_newref_array_retiring_jit(L)",
        "exercise_newref_hash_retiring_jit(L)",
        "t-jit-forward-store OK"
      }, "table-store helper")

      local tabc = t:path("src", "lj_tab.c")
      local array = t:c_block(tabc, "lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent,")
      if contains(array, "copyTVrel(L, dst, src)") or
         not contains(array, "JIT array store saw FORWARD after key/current routing.") or
         not contains(array, "lj_gc2_barrier_weak_write(L, parent, NULL, dst)") then
        error("JIT array table-store helper must resolve stale generations by key before CAS", 2)
      end
      t:assert_text_ordered("lj_tab_storetv_forjit_array", array, {
        "tab_current_jit_array_slot(L, parent, orig, key)",
        "lj_tab_trystoretv_cas(L, dst, src)"
      })
      local hash = t:c_block(tabc, "lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,")
      if contains(hash, "copyTVrel(L, dst, src)") or
         not contains(hash, "JIT hash store saw FORWARD after key/current routing.") or
         not contains(hash, "lj_gc2_barrier_weak_write(L, parent, barrier_key, dst)") then
        error("JIT hash table-store helper must resolve stale generations by key before CAS", 2)
      end
      t:assert_text_ordered("lj_tab_storetv_forjit_hash", hash, {
        "tab_current_jit_hash_slot(L, parent, orig, key, &keycopy,",
        "lj_tab_trystoretv_cas(L, dst, src)"
      })
      local newref = t:c_block(tabc, "lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,")
      if contains(newref, "copyTVrel(L, dst, src)") or
         not contains(newref, "JIT NEWREF store saw FORWARD after key resolve") or
         not contains(newref, "lj_gc2_barrier_weak_write(L, parent, key, dst)") then
        error("JIT NEWREF table-store helper must resolve by key before CAS retry", 2)
      end
      t:assert_text_ordered("lj_tab_storetv_forjit_newref", newref, {
        "dst = lj_tab_set(L, parent, key)",
        "lj_tab_trystoretv_cas(L, dst, src)"
      })

      build_and_run_c(t, t:tmp("lj_t-jit-forward-store"),
                      "t-jit-forward-store.c", { build = false })
      luajit_code(t, table_store_smoke())

      local hash_ir = t:tmp("lj-m6-hstore-ir.dump")
      luajit_dump(t, hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = { stable = 0 }
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local hash store did not trace")
]=])
      assert_dump_all_contains(t, hash_ir, { "TDUP", "HSTORE", "XPOLL" },
                               "trace-local hash store")

      local new_hash_ir = t:tmp("lj-m6-new-hstore-ir.dump")
      luajit_dump(t, new_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local new hash store did not trace")
]=])
      assert_dump_all_contains(t, new_hash_ir, { "TNEW", "NEWREF", "HSTORE", "XPOLL" },
                               "trace-local new hash store")

      local old_nil_hash_ir = t:tmp("lj-m6-oldnil-hstore-ir.dump")
      luajit_dump(t, old_nil_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
h.stable = nil
for i = 1, 80 do
  h.stable = i
  h.stable = nil
end
assert(h.stable == nil)
assert(util.traceinfo(1), "previous-nil hash store did not trace")
]=])
      assert_dump_all_contains(t, old_nil_hash_ir, { "HSTORE", "TBAR", "XPOLL" },
                               "previous-nil hash store")

      local array_ir = t:tmp("lj-m6-astore-ir.dump")
      luajit_dump(t, array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = { 0 }
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local array store did not trace")
]=])
      assert_dump_all_contains(t, array_ir, { "TDUP", "ASTORE", "XPOLL" },
                               "trace-local array store")

      local new_array_ir = t:tmp("lj-m6-new-array-hstore-ir.dump")
      luajit_dump(t, new_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local new numeric store did not trace")
]=])
      assert_dump_all_contains(t, new_array_ir, { "TNEW", "NEWREF", "HSTORE", "XPOLL" },
                               "trace-local new numeric store")

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local t = {}
for k in pairs(package) do
  local s = tostring(k)
  t[#t+1] = s
  assert(t[#t] == s and type(t[#t]) == "string",
         "numeric NEWREF helper crossed src/key TValue temps")
end
assert(util.traceinfo(1), "numeric NEWREF append did not trace")
]=])

      local old_nil_array_ir = t:tmp("lj-m6-oldnil-astore-ir.dump")
      luajit_dump(t, old_nil_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0, nil, 0 }
for i = 1, 80 do
  a[2] = i
  a[2] = nil
end
assert(a[2] == nil)
assert(util.traceinfo(1), "previous-nil array store did not trace")
]=])
      assert_dump_all_contains(t, old_nil_array_ir, { "ASTORE", "XPOLL" },
                               "previous-nil array store")

      local shared_hash_ir = t:tmp("lj-m6-shared-hstore-ir.dump")
      luajit_dump(t, shared_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i
end
assert(h.stable == 80)
assert(util.traceinfo(1), "shared existing hash store did not trace")
]=])
      assert_dump_all_contains(t, shared_hash_ir, { "HSTORE", "XPOLL" },
                               "shared existing hash store")

      local shared_array_ir = t:tmp("lj-m6-shared-astore-ir.dump")
      luajit_dump(t, shared_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0 }
for i = 1, 80 do
  a[1] = i
end
assert(a[1] == 80)
assert(util.traceinfo(1), "shared existing array store did not trace")
]=])
      assert_dump_all_contains(t, shared_array_ir, { "ASTORE", "XPOLL" },
                               "shared existing array store")
      print("M6 JIT table-store helper guard passed")
    end
  })

  add({
    name = "m6_jit_aref_pair_guard",
    description = "M6 x64 shared-array AREF generation-pair behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj-m6-aref-pair.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 128 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 128) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "separated shared array reads must load bounds from TabArrayHdr",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.eq == 0 and st.aref and st.aload and st.xpoll
      end)

      local split = t:tmp("lj-m6-aref-pair-split.dump")
      luajit_dump(t, split, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = { 1, 2, 3, 4 }
for i = 5, 64 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 64) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, split,
                       "split-from-colocated arrays must use header bounds after publish",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.eq == 0 and st.aref and st.aload and st.xpoll
      end)

      local colo = t:tmp("lj-m6-aref-pair-colo.dump")
      luajit_dump(t, colo, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = { 1, 2, 3, 4 }
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 4) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, colo,
                       "colocated shared array reads must keep the legacy pair guard",
                       function(st)
        return st.array >= 4 and st.asize >= 2 and st.eq >= 2 and
               st.xload == 0 and st.aref and st.aload and st.xpoll
      end)

      local miss = t:tmp("lj-m6-aref-pair-miss.dump")
      luajit_dump(t, miss, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 128 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = 160 + (i % 2)
  if t[k] == nil then s = s + 1 end
end
assert(s == 80)
]=], { timeout = "20s" })
      assert_trace1_ir(t, miss,
                       "separated shared out-of-array guards must load bounds from TabArrayHdr",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.ule and st.href and not st.aref and st.xpoll
      end)

      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 32 do t[i] = i end
jit.on()
local idx = 1
local function read(n)
  local s = 0
  for _ = 1, n do
    s = s + (t[idx] or 0)
  end
  return s
end
assert(read(80) == 80)
assert(util.traceinfo(1), "shared array read did not trace")
jit.off()
for i = 33, 128 do t[i] = i end
jit.on()
idx = 64
assert(read(80) == 5120)
assert(util.traceinfo(1), "trace missing after array grow")
]=], { timeout = "20s" })
      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local keep = {}
for i = 1, 120 do
  local t = {}
  for j = 1, 80 do
    t[j] = "value-" .. i .. "-" .. j
  end
  keep[i] = t
end
assert(#keep == 120 and keep[120][80] == "value-120-80")
]=], { timeout = "20s" })
      check_m6_aggregate(t, "m6_jit_aref_pair_guard.sh")
      print("M6 JIT shared AREF generation-pair behavior passed")
    end
  })

  add({
    name = "m6_jit_hrefk_nodehdr",
    description = "M6 x64 HREFK node-header behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj-m6-hrefk-ir.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local s = 0
for i = 1, 60 do
  s = s + t.foo
end
assert(s == 60)
]=])
      assert_trace1_ir(t, dump,
                       "TRACE 1 HREFK must use tab.node without tab.hmask mirror guard",
                       function(st)
        return st.node and st.hrefk and st.xpoll and not st.hmask
      end)
      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local function run(n)
  local s = 0
  for i = 1, n do
    s = s + t.foo
  end
  return s
end
assert(run(80) == 80)
for i = 1, 2000 do
  t["resize_" .. i] = i
end
t.foo = 7
assert(run(80) == 560)
]=])
      print("M6 JIT HREFK node-header behavior passed")
    end
  })

  add({
    name = "m6_jit_href_nodehdr",
    description = "M6 x64 dynamic HREF node-header behavior",
    run = function(t)
      local dump = t:tmp("lj-m6-href-nodehdr.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.opt.start("hotloop=1", "hotexit=1")
local keys = {"a", "b"}
local t = {a = 10, b = 20}
local s = 0
local function f(k)
  return t[k]
end
for i = 1, 80 do
  s = s + f(keys[i % 2 + 1])
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "dynamic string-key lookup must record HREF, not HREFK",
                       function(st) return st.href and not st.hrefk end)

      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = {}
local keys = {"missing_a", "missing_b"}
local seen = 0
for i = 1, 80 do
  local k = keys[i % 2 + 1]
  if t[k] == nil then seen = seen + 1 end
end
assert(seen == 80)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "x64 empty-hash miss must fall through to HREF without tab.hmask",
                       function(st) return st.href and not st.hrefk and not st.hmask end)
      check_m6_aggregate(t, "m6_jit_href_nodehdr.sh")
      print("M6 JIT dynamic HREF node-header behavior passed")
    end
  })

  add({
    name = "m6_jit_alloc_account",
    description = "M6 allocator accounting bridge for JIT GCSTEP removal",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_atomic.h"),
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_gc.h"),
        t:path("src", "lj_gc2.h"),
        t:path("src", "lj_gc2.c"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_safepoint.c"),
        t:path("src", "lj_tg.c"),
        t:path("src", "lj_tg.h"),
        t:path("src", "lj_api.c"),
        t:path("src", "vm_x64.dasc")
      }, {
        "uint64_t alloc_since_trigger",
        "uint64_t trigger_bytes",
        "uint64_t hard_bytes",
        "uint32_t cycle_leader",
        "uint64_t cycle_requests",
        "uint64_t cycle_starts",
        "uint64_t assist_runs",
        "uint64_t assist_grey_drained",
        "uint64_t assist_ssb_converted",
        "uint64_t assist_weak_drained",
        "uint64_t jit_hard_checks",
        "uint64_t interp_hard_checks",
        "uint32_t gcpause_pct",
        "uint32_t assist_shift",
        "uint32_t assist_active",
        "uint64_t local_total",
        "LJ_GC2_ACCT_FLUSH",
        "la_xchg64_acqrel",
        "int lj_gc_should_step(global_State *g)",
        "la_load64_acq(&g->gc2.alloc_since_trigger) >",
        "lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes)",
        "lj_gc2_flush_alloc(global_State *g, TGState *tg)",
        "lj_gc2_update_pacing(global_State *g)",
        "lj_gc2_assist_shift_from_stepmul(uint32_t stepmul)",
        "lj_gc2_assist(global_State *g, TGState *tg)",
        "static int gc2_request_cycle(global_State *g, TGState *tg)",
        "static void gc2_maybe_trigger_cycle(global_State *g, TGState *tg)",
        "la_cas32(&g->gc2.cycle_leader",
        "la_add64_rlx(&g->gc2.cycle_requests",
        "la_add64_rlx(&g->gc2.cycle_starts",
        "lj_gc_threshold_load(g) == LJ_MAX_MEM",
        "lj_gc_threshold_store(g, g->gc.total)",
        "la_store64_rel(&g->gc2.trigger_bytes",
        "la_store64_rel(&g->gc2.hard_bytes",
        "la_cas32(&g->gc2.assist_active",
        "tg->gc_assist = 1",
        "lj_gc_step_top(lua_State *L)",
        "call extern lj_gc_step_top",
        "lj_gc_step_top(L)",
        "legacy_step = g->gc.total >= lj_gc_threshold_load(g)",
        "la_add64_rlx(&g->gc2.assist_runs",
        "la_add64_rlx(&g->gc2.interp_hard_checks",
        "lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 interpreter assist bridge. */",
        "if (legacy_step)",
        "la_add64_rlx(&g->gc2.assist_grey_drained",
        "la_add64_rlx(&g->gc2.assist_ssb_converted",
        "la_add64_rlx(&g->gc2.assist_weak_drained",
        "la_store64_rlx(&g->gc2.jit_hard_checks, 0)",
        "la_store64_rlx(&g->gc2.interp_hard_checks, 0)",
        "la_xchg32_acqrel(&g->gc2.cycle_leader, 0)",
        "gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg",
        "gc2_drain_published_ssb_to_grey(global_State *g",
        "gc2_drain_grey(g, left)",
        "lj_gc2_weak_drain(g, limit - work)",
        "05 section 5.11",
        "static void gc2_reset_alloc_trigger(global_State *g)",
        "la_add64_rlx(&tg->local_total",
        "la_add64_rlx(&g->gc2.alloc_since_trigger",
        "la_store64_rlx(&g->gc2.alloc_since_trigger, 0)",
        "lj_gc2_account_alloc(g, L2TG(L), nsz - osz)",
        "lj_gc2_account_alloc(g, L2TG(L), size)",
        "lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 safepoint flush. */",
        "lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 detach accounting. */",
        "la_store32_rel(&g->gc2.gcpause_pct",
        "la_store32_rel(&g->gc2.assist_shift"
      }, "allocator accounting")
      if count_plain(t:read(t:path("src", "vm_x64.dasc")),
                     "GL:ITYPE->gc2.alloc_since_trigger") < 3 then
        error("x64 TNEW/TDUP/ffgccheck must check GC2 hard allocation threshold", 2)
      end
      check_m6_aggregate(t, "m6_jit_alloc_account.sh")
      build_and_run_c(t, t:tmp("lj_t-gc2-alloc-account"),
                      "t-gc2-alloc-account.c", { build = false, timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-gc2-interp-hard-check"),
                      "t-gc2-interp-hard-check.c", { build = false, timeout = "20s" })
      print("M6 JIT allocator accounting guard passed")
    end
  })

  add({
    name = "m6_jit_gc2_readiness",
    description = "GC2 allocation-pacing readiness while JIT GCSTEP is live",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_gc2.c"),
        t:path("src", "lj_gc2.h"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc.h"),
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_ir.h"),
        t:path("src", "lj_ircall.h"),
        t:path("src", "lj_snap.c"),
        t:path("src", "lj_asm.c"),
        t:path("src", "lj_asm_x86.h")
      }, {
        "uint32_t cycle_leader",
        "uint64_t cycle_requests",
        "uint64_t cycle_starts",
        "uint64_t jit_hard_checks",
        "static int gc2_request_cycle(global_State *g, TGState *tg)",
        "static void gc2_maybe_trigger_cycle(global_State *g, TGState *tg)",
        "la_cas32(&g->gc2.cycle_leader",
        "la_add64_rlx(&g->gc2.cycle_requests",
        "la_add64_rlx(&g->gc2.cycle_starts",
        "lj_gc_threshold_load(g) == LJ_MAX_MEM",
        "lj_gc_threshold_store(g, g->gc.total)",
        "lj_gc2_assist(global_State *g, TGState *tg)",
        "lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 trace-side assist bridge. */",
        "legacy_step = g->gc.total >= lj_gc_threshold_load(g)",
        "la_add64_rlx(&g->gc2.jit_hard_checks",
        "la_cas32(&g->gc2.assist_active",
        "gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg",
        "gc2_drain_published_ssb_to_grey(global_State *g",
        "emit_getgl(as, tmp, gc2.alloc_since_trigger)",
        "emit_opgl(as, XO_ARITH(XOg_CMP), tmp|REX_GC64, gc2.hard_bytes)",
        "checkmclim(as);  /* M6: split trace-head GC check after snapshot prep. */",
        "checkmclim(as);  /* M6: start long GC check sequence on a fresh red zone. */",
        "checkmclim(as);  /* M6: split long GC check sequence for assert red zone. */",
        "checkmclim(as);  /* M6: split GC2-hard and legacy GC threshold tests. */",
        "lj_gc_step_jit",
        "IR_GCSTEP",
        "asm_gc_check"
      }, "GC2/JIT pacing readiness")
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-hard-check"),
                      "t-gc2-jit-hard-check.c", { build = false, timeout = "20s" })

      local tnew = t:tmp("lj_t-jit-gc2-readiness-tnew.dump")
      luajit_dump(t, tnew, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], { timeout = "20s" })
      assert_dump_all_contains(t, tnew, { "TNEW", "XPOLL", "GCSTEP" }, "TNEW readiness")

      local cnew = t:tmp("lj_t-jit-gc2-readiness-cnew.dump")
      luajit_dump(t, cnew, "-jdump=ir", [=[
local ffi=require("ffi")
ffi.cdef("typedef struct { int x; } lj_gc2_dump_cnew_t;")
local ct=ffi.typeof("lj_gc2_dump_cnew_t")
jit.opt.start("hotloop=1","hotexit=1","-sink")
local x
for i=1,100 do x=ct(i) end
assert(x.x==100)
]=], { timeout = "20s" })
      assert_dump_all_contains(t, cnew, { "CNEW", "XPOLL" }, "CNEW readiness")

      local snew = t:tmp("lj_t-jit-gc2-readiness-snew.dump")
      luajit_dump(t, snew, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1","-sink")
local s="abcdef"
local x
for i=1,100 do x=string.sub(s,1,3) end
assert(x=="abc")
]=], { timeout = "20s" })
      assert_dump_all_contains(t, snew, { "SNEW", "XPOLL" }, "SNEW readiness")
      check_m6_aggregate(t, "m6_jit_gc2_readiness.sh")
      print("M6 JIT GC2 readiness guard passed")
    end
  })

  add({
    name = "m6_jit_gcstep_guard",
    description = "legacy JIT GC-step pacing guard",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc.h"),
        t:path("src", "lj_ir.h"),
        t:path("src", "lj_ircall.h"),
        t:path("src", "lj_snap.c"),
        t:path("src", "lj_asm.c"),
        t:path("src", "lj_asm_x86.h")
      }, {
        "lj_gc_step_jit",
        "IR_GCSTEP",
        "asm_gcstep",
        "asm_gc_check",
        "as->gcsteps++"
      }, "JIT GC-step pacing")
      local dump = t:tmp("lj_t-jit-gcstep.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], { timeout = "20s" })
      assert_dump_contains(t, dump, "GCSTEP", "sunk allocation replay")
      luajit_file(t, t:path("tests", "stock", "test", "misc", "gcstep.lua"),
                  { timeout = "20s" })
      check_m6_aggregate(t, "m6_jit_gcstep_guard.sh")
      print("M6 JIT GC-step guard passed")
    end
  })

  add({
    name = "m6_jit_mcode_publish",
    description = "Linux/x64 mcode sync-core publication ordering",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_jit.h"),
        t:path("src", "lj_mcode.h"),
        t:path("src", "lj_mcode.c"),
        t:path("src", "lj_state.c"),
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_emit_x86.h"),
        t:path("src", "lj_asm_x86.h"),
        t:path("src", "lj_err.h"),
        t:path("src", "lj_err.c")
      }, {
        "uint32_t jit_mcode_synccore",
        "void lj_mcode_init(global_State *g)",
        "la_membarrier_register_synccore() == 0",
        "la_store32_rel(&g->jit_mcode_synccore, 1)",
        "void lj_mcode_sync_core(jit_State *J)",
        "la_load32_acq(&g->jit_mcode_synccore)",
        "la_membarrier_synccore()",
        "lj_mcode_init(g);",
        "lj_mcode_sync_core(J);",
        "LJ_MCODE_EXEC_STABLE",
        "lj_mcode_freeall(global_State *g)",
        "mcode_freearea_direct(global_State *g, MCode *area, size_t size)",
        "lj_mcode_freeall(g);",
        "J->szallmcarea + sizemcode > maxmcode",
        "MCode *rw;\t\t/* Writable alias of this area. */",
        "lj_mcode_area_rw(MCode *area)",
        "lj_mcode_rx2rw(MCode *area, MCode *rx)",
        "lj_mcode_rw2rx(MCode *area, MCode *rw)",
        "lj_mcode_rw(jit_State *J, MCode *rx)",
        "LJ_MCODE_DUALMAP",
        "mcode_memfd_create(void)",
        "mcode_alloc_dualmap(uintptr_t hint, size_t sz)",
        "mmap((void *)hint, sz, MCPROT_RX, MAP_SHARED, fd, 0)",
        "mmap(NULL, sz, MCPROT_RW, MAP_SHARED, fd, 0)",
        "((MCLink *)rw)->rw = (MCode *)rw;",
        "mcode_register_area(jit_State *J, MCode *area",
        "mcode_free_mapping(MCode *area, size_t sz)",
        "lj_err_register_mcode(area, sz, (uint8_t *)bot,",
        "uint8_t *lj_err_register_mcode(void *base, size_t sz, uint8_t *info,",
        "memcpy(winfo, err_frame_jit_template, sizeof(err_frame_jit_template));",
        "memcpy(winfo + ERR_FRAME_JIT_OFS_HANDLER, &handler, sizeof(handler));",
        "rwlink->next = oldarea;",
        "rwlink->size = sz;",
        "rwlink->rw = rwarea;",
        "mcode_free_mapping(area, size);",
        "asm_mcode_u8(ASMState *as, MCode **pp, MCode v)",
        "asm_mcode_u64(ASMState *as, MCode **pp, uint64_t v)",
        "asm_mcode_i32(ASMState *as, MCode **pp, int32_t v)",
        "asm_mcode_ptr(ASMState *as, MCode **pp, const void *v)",
        "asm_mcode_mem(ASMState *as, MCode **pp,",
        "asm_mcode_put_u8(ASMState *as, MCode *p, MCode v)",
        "asm_mcode_put_u16(ASMState *as, MCode *p, uint16_t v)",
        "asm_mcode_put_i32(ASMState *as, MCode *p, int32_t v)",
        "asm_mcode_put_u32(ASMState *as, MCode *p, uint32_t v)",
        "asm_mcode_put_u64(ASMState *as, MCode *p, uint64_t v)",
        "asm_mcode_patch_i32(jit_State *J, MCode *p, int32_t v)",
        "emit_op(ASMState *as, x86Op xo",
        "emit_opm(ASMState *as, x86Op xo",
        "emit_opmx(ASMState *as, x86Op xo",
        "lj_mcode_rw(as->J, *pp)",
        "asm_mcode_i32(as, &mcp, jmprel(as->J, mcp + 4, target));",
        "*lj_mcode_rw(as->J, as->mctop) = XI_NOP;",
        "asm_mcode_put_i32(as, p+1, jmprel(as->J, p+5, target));",
        "asm_mcode_patch_i32(J, p+len-4, jmprel(J, p+len, target));",
        "asm_mcode_patch_i32(J, p+2, jmprel(J, p+6, target));",
        "memfd dual-map W^X write view"
      }, "mcode publication")

      local lj_mcode = t:path("src", "lj_mcode.c")
      do
        local inx64 = false
        for line in lines(t:read(lj_mcode)) do
          if contains(line, "#if defined(__linux__) && LJ_TARGET_X64") then
            inx64 = true
          elseif inx64 and contains(line, "#endif") then
            inx64 = false
          elseif inx64 and contains(line, "MCPROT_RWX") then
            error("secure Linux/x64 mcode bridge must not fall back to RWX", 2)
          end
        end
      end
      assert_no_lines(t, "mcode publication bridge must not be hidden behind LJ_MT",
                      { lj_mcode }, function(line)
        return line:match("#if%s+LJ_MT") or line:match("#ifdef%s+LJ_MT") or
               contains(line, "LUAJIT_THREADSAFE")
      end)
      assert_no_lines(t, "Linux/x64 mcode bridge must not retain the single-map scaffold",
                      { lj_mcode }, function(line)
        return contains(line, "single-map write view") or contains(line, "rw == rx") or
               contains(line, "rw = J->mcarea")
      end)
      assert_no_lines(t, "Linux/x64 mcode bridge must not force fresh areas after publication",
                      { lj_mcode }, function(line)
        return contains(line, "LJ_MCODE_FRESH_AREA") or
               contains(line, "mcode_area_has_published") or
               contains(line, "mcode_fresh_size") or
               contains(line, "mcode_allocarea_checked")
      end)
      assert_no_lines(t, "x64 mcode bottom writes must go through lj_mcode_rw helpers",
                      { t:path("src", "lj_emit_x86.h"), t:path("src", "lj_asm_x86.h") },
                      function(line)
        return contains(line, "*as->mcbot") or contains(line, "*mxp++") or
               contains(line, "*(uint64_t *)as->mcbot") or
               contains(line, "*(void **)mxp") or contains(line, "memcpy(mxp")
      end)
      assert_no_lines(t, "x64 core emitter writes must go through lj_mcode_rw helpers",
                      { t:path("src", "lj_emit_x86.h") }, function(line)
        return contains(line, "*--as->mcp") or
               raw_index_write(line, "as%->mcp") or
               raw_index_write(line, "source") or
               raw_index_write(" " .. line, "[^%w_]p") or
               raw_cast_write(line)
      end)

      do
        local bad, seen = false, false
        for line in lines(t:read(t:path("src", "lj_asm_x86.h"))) do
          if contains(line, "void lj_asm_patchexit(jit_State *J, GCtrace *T, ExitNo exitno, MCode *target)") then
            seen = true
            break
          end
          if contains(line, "*--as->mcp") or raw_index_write(line, "as%->mcp") or
             contains(line, "*--p") or
             (not contains(line, "MCode *patchnfpr") and line:match("%*patchnfpr%s*=[^=]")) or
             (not contains(line, "MCode *q") and line:match("%*q%s*[-+]?=[^=]")) or
             raw_index_write(" " .. line, "[^%w_]p") or raw_cast_write(line) then
            bad = true
            break
          end
        end
        if not seen then
          error("lj_asm_patchexit marker missing", 2)
        end
        if bad then
          error("x64 generation-time asm writes must go through lj_mcode_rw helpers", 2)
        end
      end
      assert_block_no_lines(t, "x64 trace tail fixups must go through lj_mcode_rw helpers",
                            t:path("src", "lj_asm_x86.h"),
                            "static void asm_tail_fixup(ASMState *as, TraceNo lnk)",
                            function(line)
        return contains(line, "*mcp++") or contains(line, "mcp += 4") or
               contains(line, "*(int32_t *)mcp") or
               contains(line, "*(int32_t *)(mcp-4)") or
               contains(line, "*--as->mctop")
      end)
      assert_block_no_lines(t, "x64 committed-code exit patches must go through lj_mcode_rw helpers",
                            t:path("src", "lj_asm_x86.h"),
                            "void lj_asm_patchexit(jit_State *J, GCtrace *T, ExitNo exitno, MCode *target)",
                            function(line)
        return raw_index_write(" " .. line, "[^%w_]p") or raw_cast_write(line)
      end)

      local reserve = t:c_block(lj_mcode, "MCode *lj_mcode_reserve(jit_State *J, MCode **lim)")
      t:assert_text_ordered("lj_mcode_reserve", reserve, {
        "if (!J->mcarea)",
        "mcode_allocarea(J, mcode_default_size(J))",
        "else",
        "mcode_protect(J, MCPROT_GEN)"
      })

      local stop = t:c_block(t:path("src", "lj_trace.c"), "static void trace_stop(jit_State *J)")
      assert_order_positions("trace_stop", stop, {
        "lj_mcode_commit(J, J->cur.mcode)",
        "lj_mcode_sync_core(J)",
        "trace_save(J, T)",
        "proto_trace_rel(pt, traceno)",
        "bc_publish(patchpc, patchins)",
        "trace_exittarget_rel(parent, J->exitno, T->mcode)",
        "trace_nextside_rel(root, traceno)",
        "trace_link_rel(parent, traceno)"
      })
      check_m6_aggregate(t, "m6_jit_mcode_publish.sh")

      local timeout = os.getenv("M6_MCODE_TIMEOUT") or "60s"
      luajit_code(t, [=[
jit.opt.start("hotloop=1","hotexit=1")
local s=0
for i=1,80 do s=s+i end
assert(s==3240)
]=], { timeout = timeout })
      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=8")
local function make(seed)
  return assert(loadstring(("return function(n) local s=%d; for i=1,n do s=s+i end return s end"):format(seed)))()
end
for n = 1, 12 do
  local seed = n * 17
  local f = make(seed)
  for _ = 1, 8 do
    assert(f(32) == seed + 528)
  end
end
local live = 0
for tr = 1, 40 do
  if util.traceinfo(tr) then live = live + 1 end
end
assert(live >= 8, live)
]=], { timeout = timeout })
      luajit_file(t, t:path("tests", "t-jit-mcode-fresh.lua"),
                  { lua_path = true, timeout = "30s" })
      print("M6 JIT mcode publication guard passed")
    end
  })

  add({
    name = "m6_jit_flush_hs",
    description = "JIT flush safepoint-scoped publication and retirement",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_safepoint.c"),
        t:path("src", "lj_record.c"),
        t:path("src", "lj_dispatch.c"),
        t:path("tests", "t-vm-safepoint.c")
      }, {
        "lj_trace_flushall_hs(lua_State *L)",
        "lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)",
        "lj_trace_flushall(mainthread(g));  /* 08 section 8.7 leader action. */",
        "(void)lj_trace_flushall_hs(J->L);",
        "(void)lj_trace_flushall_hs(L);",
        "uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)",
        "(void)lj_trace_flushscope(J, lnk);  /* Flush return trace after HS. */",
        "trace_scope_flush_dependency(jit_State *J, GCtrace *T)",
        "(void)trace_flushscope_mark_deps(G2J(g));",
        "trace_flushside(jit_State *J, GCtrace *T, int scoped)",
        "return trace_flushside(J, T, 1);",
        "(void)trace_flushside(J, T, 1);",
        "trace_nextside_rel(root, next);",
        "first_trace_with_root(jit_State *J, TraceNo root)",
        "call_jit_flush_trace(L, sidetrace);"
      }, "JIT flush handshake")
      assert_no_lines(t, "full trace flush callers must route through HS_FLUSHJ",
                      {
                        t:path("src", "lj_trace.c"),
                        t:path("src", "lj_record.c"),
                        t:path("src", "lj_dispatch.c"),
                        t:path("src", "lj_api.c"),
                        t:path("src", "lj_profile.c"),
                        t:path("src", "lib_ffi.c")
                      }, function(line)
        return contains(line, "lj_trace_flushall(J->L)") or
               contains(line, "lj_trace_flushall(L)")
      end)
      t:assert_not_contains(t:path("src", "lj_record.c"), "lj_trace_flush(J, lnk)")
      t:assert_not_contains(t:path("src", "lj_trace.c"), "Only root traces are considered")
      check_m6_aggregate(t, "m6_jit_flush_hs.sh")
      t:run({ t:path("tools", "ci", "m5_jit_trace_publish.sh") })
      t:run({ t:path("tools", "ci", "m3_vm_safepoint.sh") })
      luajit_file(t, t:path("tests", "stock", "test", "misc", "jit_flush.lua"))
      print("M6 JIT flush handshake guard passed")
    end
  })

  add({
    name = "m6_jit",
    description = "M6 JIT aggregate scaffold gates",
    run = function(t)
      local cmd = { t:path("tools", "ci", "lua_test.sh") }
      for i = 1, #m6_cases do cmd[#cmd + 1] = m6_cases[i] end
      t:run(cmd)
      print("M6 JIT scaffold tests passed")
    end
  })
end
