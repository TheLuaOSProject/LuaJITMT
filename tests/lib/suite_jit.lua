local runtime = require("suite_runtime")
local checks = require("suite_assert")

local M = {}

local contains = checks.contains
local lines = checks.iter_lines

function M.trace1_ir_state(t, dump)
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
    astore = false,
    hstore = false,
    hload = false,
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
      if contains(line, " ASTORE ") then st.astore = true end
      if contains(line, " HSTORE ") then st.hstore = true end
      if contains(line, " HLOAD ") then st.hload = true end
      if contains(line, " XPOLL ") or contains(line, "XPOLL") then st.xpoll = true end
      if contains(line, "tab.hmask") then st.hmask = true end
      if contains(line, "tab.node") then st.node = true end
    end
  end
  return st
end

function M.assert_trace1_ir(t, dump, label, pred)
  local st = M.trace1_ir_state(t, dump)
  if not st.done or not pred(st) then
    io.stderr:write(t:read(dump))
    error(label, 2)
  end
end

function M.x64_cmp_poll_pattern()
  return "cmp dword %[r14%+0x[0-9a-f]+%], %+0x00"
end

function M.assert_x64_loop_poll_count(t, dump, label, mincmp)
  local data = t:read(dump)
  local loop, cmp = false, 0
  for line in lines(data) do
    if contains(line, "->LOOP:") then loop = true end
    if loop and line:match(M.x64_cmp_poll_pattern()) then cmp = cmp + 1 end
  end
  if not loop or cmp < mincmp then
    io.stderr:write(data)
    error(label, 2)
  end
end

function M.assert_loop_ir_markers(t, dump, label, markers)
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

function M.assert_loop_after_xpoll(t, dump, label, markers)
  local data = t:read(dump)
  local loop, xpoll = false, false
  local seen = {}
  for line in lines(data) do
    if contains(line, "------ LOOP") then
      loop = true
    elseif loop and contains(line, "---- TRACE 1 stop") then
      break
    elseif loop then
      if contains(line, "XPOLL") then xpoll = true end
      if xpoll then
        for i = 1, #markers do
          if contains(line, markers[i]) then seen[markers[i]] = true end
        end
      end
    end
  end
  if not xpoll then
    io.stderr:write(data)
    error(label .. ": missing loop XPOLL", 2)
  end
  for i = 1, #markers do
    if not seen[markers[i]] then
      io.stderr:write(data)
      error(label .. ": missing post-XPOLL marker: " .. markers[i], 2)
    end
  end
end

function M.assert_call_after_loop_polls(t, dump, label, call, mincmp, extra)
  local data = t:read(dump)
  local loop, cmp, done = false, 0, false
  local state = {}
  for line in lines(data) do
    if contains(line, "->LOOP:") then loop = true end
    if loop and line:match(M.x64_cmp_poll_pattern()) then cmp = cmp + 1 end
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

function M.run_ir_dump_probe(t, dump, script, opts)
  opts = opts or {}
  runtime.luajit_dump(t, dump, opts.dumpopt or "-jdump=ir", script, {
    timeout = opts.timeout or "20s",
    stderr = opts.stderr == nil and false or opts.stderr,
    quiet = opts.quiet
  })
  return dump
end

function M.ir_dump_probe(t, name, script, opts)
  return M.run_ir_dump_probe(t, t:tmp(name), script, opts)
end

function M.assert_ir_dump_probe_contains(t, name, script, needle, label, opts)
  local dump = M.ir_dump_probe(t, name, script, opts)
  checks.assert_dump_contains(t, dump, needle, label)
  return dump
end

function M.assert_ir_dump_probe_all_contains(t, name, script, needles, label, opts)
  local dump = M.ir_dump_probe(t, name, script, opts)
  checks.assert_dump_all_contains(t, dump, needles, label)
  return dump
end

return M
