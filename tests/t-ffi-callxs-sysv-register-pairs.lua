local bit = require "bit"
local ffi = require "ffi"
local util = require "jit.util"
local vmdef = require "jit.vmdef"

if jit.arch ~= "x64" or jit.os ~= "Linux" then
  print("t-ffi-callxs-sysv-register-pairs SKIP: Linux x64 only")
  return
end

ffi.cdef [[
void lj_callxs_sysv_reset(void);
uint32_t lj_callxs_sysv_count(uint32_t);
void lj_callxs_sysv_set_probe_mode(uint32_t);
typedef struct { int64_t a, b; } lj_callxs_sysv_int16;
typedef struct { float a, b, c, d; } lj_callxs_sysv_sse16;
typedef struct { int64_t a; double b; } lj_callxs_sysv_mix16;
typedef struct { double a; int64_t b; } lj_callxs_sysv_reverse16;
typedef struct { int64_t a, b, c; } lj_callxs_sysv_result24;
double lj_callxs_sysv_register_pairs(double, lj_callxs_sysv_int16,
  lj_callxs_sysv_sse16, lj_callxs_sysv_mix16, lj_callxs_sysv_reverse16, int32_t);
double lj_callxs_sysv_pair_gpr_rollback(int, int, int, int, int,
  lj_callxs_sysv_int16, int);
double lj_callxs_sysv_pair_fpr_rollback(double, double, double, double, double,
  double, double, lj_callxs_sysv_sse16, double);
lj_callxs_sysv_result24 lj_callxs_sysv_pair_sret(int, int, int,
  lj_callxs_sysv_int16, int);
lj_callxs_sysv_result24 lj_callxs_sysv_pair_sret_rollback(int, int, int, int,
  lj_callxs_sysv_int16, int);
double lj_callxs_sysv_pair_vararg(lj_callxs_sysv_sse16, int, ...);
]]

local lib = ffi.load(assert(os.getenv("LJ_M7_FFI_CALLXS_SYSV_AGG_SO")))

local function trace_op_counts()
  local counts = {}
  for tr = 1, 256 do
    local info = util.traceinfo(tr)
    if info then
      for ref = 1, info.nins do
        local _, ot = util.traceir(tr, ref)
        if ot then
          local opidx = bit.rshift(ot, 8)
          local op = vmdef.irnames:sub(opidx * 6 + 1, opidx * 6 + 6)
          counts[op] = (counts[op] or 0) + 1
        end
      end
    end
  end
  return counts
end
jit.off(trace_op_counts, true)

local function result_exit_snapshots()
  local exits = {}
  for tr = 1, 256 do
    local info = util.traceinfo(tr)
    if info and info.linktype == "loop" then
      local call
      for ref = 1, info.nins do
        local _, ot, lhs, rhs = util.traceir(tr, ref)
        local op = bit.rshift(ot, 8) * 6
        local name = vmdef.irnames:sub(op + 1, op + 6)
        if name == "CALLXS" then
          call = ref
        elseif call and name == "EQ    " and (lhs == call or rhs == call) then
          for sn = info.nexit - 1, 0, -1 do
            if util.tracesnap(tr, sn)[0] <= ref then
              exits[tr] = exits[tr] or {}
              exits[tr][sn] = true
              break
            end
          end
        end
      end
    end
  end
  return exits
end
jit.off(result_exit_snapshots)

local function reset_effects()
  lib.lj_callxs_sysv_reset()
end
jit.off(reset_effects, true)

local function reset_case()
  jit.flush()
  reset_effects()
end
jit.off(reset_case, true)

local function assert_case(which, expected, label)
  assert(lib.lj_callxs_sysv_count(which) == expected,
         label .. " replayed or omitted a native effect")
  local counts = trace_op_counts()
  assert((counts["XSAVE "] or 0) > 0, label .. " omitted XSAVE")
  assert((counts["CALLXS"] or 0) > 0, label .. " omitted CALLXS")
end
jit.off(assert_case, true)

jit.opt.start("hotloop=1", "hotexit=1")
local n = 96

local register_pairs = lib.lj_callxs_sysv_register_pairs
local gpr_rollback = lib.lj_callxs_sysv_pair_gpr_rollback
local fpr_rollback = lib.lj_callxs_sysv_pair_fpr_rollback
local pair_sret = lib.lj_callxs_sysv_pair_sret
local sret_rollback = lib.lj_callxs_sysv_pair_sret_rollback
local pair_vararg = lib.lj_callxs_sysv_pair_vararg
local set_probe_mode = lib.lj_callxs_sysv_set_probe_mode
local ii = ffi.new("lj_callxs_sysv_int16", 2, 3)
local ss = ffi.new("lj_callxs_sysv_sse16", 4, 5, 6, 7)
local is = ffi.new("lj_callxs_sysv_mix16", 8, 9.5)
local si = ffi.new("lj_callxs_sysv_reverse16", 10.25, 11)
local expected = 1.5 + 2*2 + 3*3 + 5*4 + 7*5 + 11*6 + 13*7 +
                 17*8 + 19*9.5 + 23*10.25 + 29*11 + 12

local function run_register_pairs(count, func, ii, ss, is, si)
  local mismatches = 0
  for _ = 1, count do
    local got = func(1.5, ii, ss, is, si, 12)
    if got ~= expected then
      assert(got == expected + 1)
      mismatches = mismatches + 1
    end
  end
  return mismatches
end

local function run_gpr_rollback(count, func, arg)
  local sum = 0
  for _ = 1, count do sum = sum + func(1, 2, 3, 4, 5, arg, 19) end
  return sum
end

local function run_fpr_rollback(count, func, arg)
  local sum = 0
  for _ = 1, count do sum = sum + func(1, 2, 3, 4, 5, 6, 7, arg, 21) end
  return sum
end

local function run_sret(count, func, arg)
  local result
  for _ = 1, count do result = func(1, 2, 3, arg, 19) end
  return result
end

local function run_sret_rollback(count, func, arg)
  local result
  for _ = 1, count do result = func(1, 2, 3, 4, arg, 19) end
  return result
end

local function run_vararg(count, func, arg)
  local sum = 0
  for _ = 1, count do
    sum = sum + func(arg, 7, 1, 2, 3, 4, 5, 6, 7)
  end
  return sum
end

local threading = require("threading")
local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(ready, stop)
  assert(ready:send(true, 10))
  assert(select(2, stop:recv(20)))
  return true
end, ready, stop))

local function check_pairs()
  assert(select(2, ready:recv(10)))
  reset_case()
  assert(run_register_pairs(n, register_pairs, ii, ss, is, si) == 0)
  assert_case(14, n, "two-eightbyte mixed register arguments")
  collectgarbage("collect")
  reset_effects()
  set_probe_mode(1)
  local exits, executed = result_exit_snapshots(), false
  local function observe(tr, sn)
    if exits[tr] and exits[tr][sn] then executed = true end
  end
  jit.off(observe)
  jit.attach(observe, "texit")
  local called, mismatches = pcall(run_register_pairs, n,
                                  register_pairs, ii, ss, is, si)
  jit.attach(observe)
  assert(called and mismatches == n)
  assert(executed, "native result did not reach its caller-side guard")
  assert_case(14, n, "post-call exit with two-eightbyte arguments")

  reset_case()
  assert(run_gpr_rollback(n, gpr_rollback, ii) == n * (15 + 4 + 9 + 95))
  assert(lib.lj_callxs_sysv_count(15) == n)
  assert((trace_op_counts().CALLXS or 0) == 0, "partial GPR pair compiled")

  reset_case()
  assert(run_fpr_rollback(n, fpr_rollback, ss) ==
    n * (28 + 4 + 2*5 + 3*6 + 4*7 + 105))
  assert(lib.lj_callxs_sysv_count(16) == n)
  assert((trace_op_counts().CALLXS or 0) == 0, "partial FPR pair compiled")

  reset_case()
  local result = run_sret(n, pair_sret, ii)
  assert(result.a == 6 and result.b == 8 and result.c == 19)
  assert_case(17, n, "register pair with hidden sret")

  reset_case()
  result = run_sret_rollback(n, sret_rollback, ii)
  assert(result.a == 10 and result.b == 8 and result.c == 19)
  assert(lib.lj_callxs_sysv_count(18) == n)
  assert((trace_op_counts().CALLXS or 0) == 0, "missing hidden sret GPR reserve")

  reset_case()
  assert(run_vararg(n, pair_vararg, ss) == n * (4 + 2*5 + 3*6 + 4*7 + 140))
  assert_case(19, n, "fixed SSE pair and variadic register/stack arguments")
end
jit.off(check_pairs)

local ok, err = xpcall(check_pairs, debug.traceback)
assert(stop:send(true, 10))
local joined, result = peer:join(10)
assert(ok, err)
assert(joined and result)
print("t-ffi-callxs-sysv-register-pairs OK")
