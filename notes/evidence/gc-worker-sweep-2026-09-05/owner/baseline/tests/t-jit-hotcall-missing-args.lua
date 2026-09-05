local bit = require("bit")
local util = require("jit.util")
local vmdef = require("jit.vmdef")

local function trace_ir_counts(tr)
  local info = assert(util.traceinfo(tr), "missing trace")
  local sloads = 0
  local guarded_sloads = 0
  local guarded_nil_sloads = 0
  for ref = 1, info.nins do
    local _, ot = util.traceir(tr, ref)
    if ot then
      local opidx = bit.rshift(ot, 8)
      local opname = vmdef.irnames:sub(opidx * 6 + 1, opidx * 6 + 6)
      if opname == "SLOAD " then
        sloads = sloads + 1
        if bit.band(ot, 0x80) ~= 0 then
          guarded_sloads = guarded_sloads + 1
          if bit.band(ot, 0x1f) == 0 then
            guarded_nil_sloads = guarded_nil_sloads + 1
          end
        end
      end
    end
  end
  return info.nins, sloads, guarded_sloads, guarded_nil_sloads
end

jit.off(trace_ir_counts, true)

local function missing(a, b, c, d)
  return d == nil and c or d
end

local function drive_missing()
  local prefix = "x"
  local out
  for i = 1, 200 do
    -- CAT leaves string scratch immediately above the three real arguments.
    -- A hot-call recorder must not sample it as the missing fourth parameter.
    out = missing("a", "b", prefix .. "y" .. i)
  end
  return out
end

jit.off(drive_missing)
jit.flush()
jit.opt.start("hotloop=56", "hotexit=10")
assert(drive_missing() == "xy200")
assert(util.traceinfo(1), "missing-argument hotcall did not trace")

local function drive_alternating()
  local prefix = "x"
  local out
  for i = 1, 200 do
    if i % 2 == 0 then
      out = missing("a", "b", prefix .. "y" .. i, "real")
      assert(out == "real")
    else
      out = missing("a", "b", prefix .. "y" .. i)
      assert(out == prefix .. "y" .. i)
    end
  end
  return out
end

jit.off(drive_alternating)
-- Keep the missing-argument trace live: a supplied fourth argument must fail
-- its entry NIL guard and side-exit without being overwritten by snapshot 0.
assert(drive_alternating() == "real")
assert(util.traceinfo(1), "alternating-argument hotcall lost its trace")

local escaped_capture

local function capture_missing(a, b, c, d)
  -- The parent never executes CGET d. FNEW is the only consumer of the
  -- missing parameter, through the child's mutable local-cell upvalue.
  escaped_capture = function(value)
    if value ~= nil then d = value end
    return d
  end
  -- A direct return needs UCLO, which is not recordable at a hot-call root.
  -- Error unwinding closes the escaped capture after the entry trace has run.
  error("capture-stop", 0)
end

local function drive_capture_missing()
  local prefix = "x"
  for i = 1, 200 do
    -- Leave string scratch where the missing fourth parameter would reside.
    local ok, err = pcall(capture_missing, "a", "b", prefix .. "y" .. i)
    assert(not ok and err == "capture-stop")
  end
  return escaped_capture
end

jit.off(drive_capture_missing)
jit.flush()
local captured = drive_capture_missing()
assert(captured() == nil, "FNEW captured stale missing-argument scratch")
assert(util.traceinfo(1), "missing FNEW-capture hotcall did not trace")
local _, _, _, capture_nil_guards = trace_ir_counts(1)
assert(capture_nil_guards == 0,
       "FNEW-only capture emitted a missing-argument NIL guard")

-- Keep that function-entry trace live. A later real argument must be captured
-- as-is, while a subsequent missing argument must still be cleared to nil.
local ok, err = pcall(capture_missing, "a", "b", "c", "real")
assert(not ok and err == "capture-stop")
captured = escaped_capture
assert(captured() == "real", "FNEW capture lost a later real argument")
ok, err = pcall(capture_missing, "a", "b", "c")
assert(not ok and err == "capture-stop")
captured = escaped_capture
assert(captured() == nil, "FNEW capture retained a later missing argument")
assert(util.traceinfo(1), "alternating FNEW-capture hotcall lost its trace")

local function all_missing(a, b, c, d)
  return a == nil and b == nil and c == nil and d == nil
end

local function drive_all_missing()
  local out
  for _ = 1, 200 do
    out = all_missing()
  end
  return out
end

jit.off(drive_all_missing)
jit.flush()
assert(drive_all_missing())
assert(util.traceinfo(1), "all-missing-argument hotcall did not trace")

local function make_high_arity(nparams)
  local names = {}
  for i = 1, nparams do
    names[i] = "p" .. i
  end
  local source = "return function(" .. table.concat(names, ",") ..
                 ") return p128 == nil and p1 or -1 end"
  return assert(loadstring(source, "=hotcall-high-arity"))()
end

local high_arity = make_high_arity(128)

local function drive_high_arity()
  local out
  for i = 1, 200 do
    -- p128 is the only consumed missing parameter. p2..p127 must not acquire
    -- entry guards merely because they belong to the missing suffix.
    out = high_arity(i)
  end
  return out
end

jit.off(drive_high_arity)
jit.flush()
assert(drive_high_arity() == 200)
local nins, sloads, guarded_sloads, guarded_nil_sloads = trace_ir_counts(1)
assert(nins <= 12,
       "unused missing arguments inflated trace IR: " .. nins)
assert(sloads == 2,
       "high-arity demand trace did not emit two SLOADs: " .. sloads)
assert(guarded_sloads == 2,
       "high-arity demand trace did not emit two entry guards: " ..
       guarded_sloads)
assert(guarded_nil_sloads == 1,
       "used missing argument did not emit exactly one NIL guard: " ..
       guarded_nil_sloads)

print("t-jit-hotcall-missing-args OK")
