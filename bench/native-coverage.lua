-- Report trace publication, observed native exits, and recording refusals.
-- Usage: luajit bench/native-coverage.lua workload.lua [workload arguments...]
-- This observes execution and changes timing; use a separate run for timing.
-- Workloads must leave the trace/texit observer hooks to this wrapper.
local jit = require('jit')
local util = require('jit.util')
local vmdef = require('jit.vmdef')
local workload = assert(arg[1], 'expected workload.lua')
local fn = assert(loadfile(workload))
local traces, active, refusals = {}, {}, {}
local started, stopped, aborted, exits, flushes = 0, 0, 0, 0, 0

local function location(func, pc)
  if type(func) ~= 'function' then return '?' end
  local info = util.funcinfo(func, pc)
  return info.loc or info.source or '?'
end
local function reason(code, detail)
  if type(code) ~= 'number' then return tostring(code) end
  if type(detail) == 'function' then detail = location(detail) end
  local format = vmdef.traceerr[code] or 'unknown recording error'
  if format == 'NYI: bytecode %s' and type(detail) == 'number' then
    detail = vmdef.bcnames:sub(detail * 6 + 1, detail * 6 + 6)
  end
  local ok, message = pcall(string.format, format, detail)
  return ok and message or (format .. ' (' .. tostring(detail) .. ')')
end
local function trace(what, no, func, pc, parent, exitno)
  if what == 'flush' then
    flushes = flushes + 1
    active = {}
  elseif what == 'start' then
    started = started + 1
    active[no] = { no = no, source = location(func, pc), parent = parent,
                   parent_exit = exitno, exits = {}, total_exits = 0 }
  elseif what == 'abort' then
    aborted = aborted + 1
    local key = location(func, pc) .. ': ' .. reason(parent, exitno)
    refusals[key] = (refusals[key] or 0) + 1
    active[no] = nil
  elseif what == 'stop' then
    local row = active[no]
    if row then
      stopped = stopped + 1
      local info = assert(util.traceinfo(no))
      local code = assert(util.tracemc(no))
      row.nins, row.link, row.linktype = info.nins, info.link, info.linktype
      row.bytes = #code
      traces[#traces + 1] = row
    end
  end
end
local function exit(no, exitno)
  exits = exits + 1
  local row = active[no]
  if row then
    row.total_exits = row.total_exits + 1
    row.exits[exitno] = (row.exits[exitno] or 0) + 1
  end
end
jit.off(trace, true)
jit.off(exit, true)
local args = {}
for i = 2, #arg do args[i-1] = arg[i] end
args[0] = workload
arg = args
jit.attach(trace, 'trace')
jit.attach(exit, 'texit')
-- jit.attach has one callback per event. Silently replacing either observer
-- would make a workload report zero exits even while taking guards repeatedly.
local attach = jit.attach
jit.attach = function(callback, event)
  if event == 'trace' or event == 'texit' then
    error('native-coverage owns jit.attach trace/texit hooks', 2)
  end
  return attach(callback, event)
end
local ok, err = pcall(fn, unpack(args))
jit.attach = attach
attach(trace)
attach(exit)
if not ok then error('coverage incomplete: ' .. tostring(err), 0) end
print(string.format('coverage arch=%s starts=%d published=%d aborted=%d observed_exits=%d flushes=%d',
                    jit.arch, started, stopped, aborted, exits, flushes))
for _, row in ipairs(traces) do
  local counts = {}
  for no, count in pairs(row.exits) do counts[#counts+1] = {no, count} end
  table.sort(counts, function(a, b) return a[1] < b[1] end)
  local labels = {}
  for _, item in ipairs(counts) do
    labels[#labels+1] = string.format('%d:%d', item[1], item[2])
  end
  print(string.format('trace=%d source=%s parent=%s/%s ir=%d bytes=%d link=%s/%s observed_exits=%d [%s]',
    row.no, row.source, tostring(row.parent or 0), tostring(row.parent_exit or 0),
    row.nins, row.bytes, row.linktype, tostring(row.link), row.total_exits,
    table.concat(labels, ',')))
end
local keys = {}
for key in pairs(refusals) do keys[#keys+1] = key end
table.sort(keys)
for _, key in ipairs(keys) do
  print(string.format('refused=%d %s', refusals[key], key))
end
