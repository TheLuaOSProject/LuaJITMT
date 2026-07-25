-- FFI call and callback ABI for vector values.
--
-- Builds a small C helper library at test time and compares its results with
-- the Lua ones. Skips cleanly if no C compiler is available.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

local tmp = os.getenv("TMPDIR") or "/tmp"
local csrc = tmp .. "/luajit_simd_abi.c"
local clib = tmp .. "/luajit_simd_abi.so"

local CSRC = [[
typedef float	 float4  __attribute__((vector_size(16)));
typedef double	 double2 __attribute__((vector_size(16)));
typedef int	 i32x4   __attribute__((vector_size(16)));
typedef signed char i8x16 __attribute__((vector_size(16)));

float4 simdt_addf4(float4 a, float4 b) { return a + b; }
double2 simdt_addd2(double2 a, double2 b) { return a + b; }
i32x4 simdt_addi4(i32x4 a, i32x4 b) { return a + b; }
i8x16 simdt_addi8(i8x16 a, i8x16 b) { return a + b; }

/* Enough vector arguments to spill past the register class. */
float4 simdt_sum10(float4 a, float4 b, float4 c, float4 d, float4 e,
		   float4 f, float4 g, float4 h, float4 i, float4 j)
{
  return a + b + c + d + e + f + g + h + i + j;
}

/* Mixed integer, floating-point and vector arguments. */
float4 simdt_mixed(int n, float4 a, double x, float4 b, long long m, float4 c)
{
  float4 r = a + b + c;
  r[0] += (float)n + (float)x + (float)m;
  return r;
}

float simdt_lane(float4 a, int i) { return a[i]; }

void simdt_store(float4 *p, float4 v) { *p = v; }
float4 simdt_load(const float4 *p) { return *p; }

/* Callbacks. */
typedef void (*simdt_cbp)(float4 *, const float4 *, const float4 *);
void simdt_callcbp(simdt_cbp cb, float4 *r, const float4 *a, const float4 *b)
{ cb(r, a, b); }

typedef float4 (*simdt_cb2)(float4, float4);
float4 simdt_callcb(simdt_cb2 cb, float4 a, float4 b) { return cb(a, b); }
]]

local function build()
  local fh = io.open(csrc, "w")
  if not fh then return false end
  fh:write(CSRC)
  fh:close()
  local cc = os.getenv("CC") or "cc"
  local cmd = string.format("%s -O1 -shared -fPIC -o %q %q 2>/dev/null",
			    cc, clib, csrc)
  local ok = os.execute(cmd)
  return ok == 0 or ok == true
end

if not build() then
  test("C helper library", function()
    check(true, "no C compiler available, ABI tests skipped")
  end)
  return T
end

ffi.cdef[[
float4  simdt_addf4(float4 a, float4 b);
double2 simdt_addd2(double2 a, double2 b);
i32x4   simdt_addi4(i32x4 a, i32x4 b);
i8x16   simdt_addi8(i8x16 a, i8x16 b);
float4  simdt_sum10(float4, float4, float4, float4, float4,
		    float4, float4, float4, float4, float4);
float4  simdt_mixed(int n, float4 a, double x, float4 b, long long m, float4 c);
float   simdt_lane(float4 a, int i);
void    simdt_store(float4 *p, float4 v);
float4  simdt_load(const float4 *p);
typedef void (*simdt_cbp)(float4 *, const float4 *, const float4 *);
void    simdt_callcbp(simdt_cbp cb, float4 *r, const float4 *a, const float4 *b);
typedef float4 (*simdt_cb2)(float4, float4);
float4  simdt_callcb(simdt_cb2 cb, float4 a, float4 b);
]]

local C = ffi.load(clib)

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

test("vector argument and return", function()
  local cases = {
    {"float4", C.simdt_addf4}, {"double2", C.simdt_addd2},
    {"i32x4", C.simdt_addi4}, {"i8x16", C.simdt_addi8},
  }
  for _, c in ipairs(cases) do
    local ti = T.T[c[1]]
    local rnd = T.rng(SEED + ti.bits)
    for _ = 1, 40 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      checkeq(c[2](a, b), a + b, ti.name .. " C add")
    end
  end
end)

test("many vector arguments spill to the stack", function()
  local f4 = T.T.float4
  local rnd = T.rng(SEED + 1)
  local v = {}
  for i = 1, 10 do v[i] = T.randfinite(f4, rnd) end
  local want = f4.ct(0)
  for i = 1, 10 do want = want + v[i] end
  local got = C.simdt_sum10(v[1], v[2], v[3], v[4], v[5],
			    v[6], v[7], v[8], v[9], v[10])
  checkeq(got, want, "10 vector arguments")
end)

test("mixed scalar and vector arguments", function()
  local f4 = T.T.float4.ct
  local a, b, c = f4(1, 2, 3, 4), f4(10, 20, 30, 40), f4(100, 200, 300, 400)
  local got = C.simdt_mixed(7, a, 0.5, b, 9LL, c)
  local want = a + b + c
  want = simd.insert(want, 0, want[0] + 7 + 0.5 + 9)
  checkeq(got, want, "mixed argument list")
end)

test("scalar return from a vector argument", function()
  local f4 = T.T.float4.ct
  local a = f4(1.5, -2.5, 3.5, -4.5)
  for i = 0, 3 do
    checkeq(C.simdt_lane(a, i), a[i], "lane " .. i)
  end
end)

test("memory round trip through C", function()
  local ti = T.T.float4
  local buf = ffi.new(ffi.typeof("$[4]", ti.ct))
  local rnd = T.rng(SEED + 2)
  for i = 0, 3 do
    local v = T.rand(ti, rnd)
    C.simdt_store(buf + i, v)
    checkeq(buf[i], v, "store " .. i)
    checkeq(C.simdt_load(buf + i), v, "load " .. i)
  end
end)

test("callback taking vectors by pointer", function()
  local f4 = T.T.float4
  local cb = ffi.cast("simdt_cbp", function(r, a, b) r[0] = a[0] * b[0] end)
  local box = ffi.new(ffi.typeof("$[3]", f4.ct))
  local rnd = T.rng(SEED + 3)
  for _ = 1, 20 do
    local a, b = T.randfinite(f4, rnd), T.randfinite(f4, rnd)
    box[1], box[2] = a, b
    C.simdt_callcbp(cb, box, box + 1, box + 2)
    checkeq(box[0], a * b, "vector callback through pointers")
  end
  cb:free()
end)

test("callbacks passing vectors by value are rejected cleanly", function()
  -- LuaJIT's callback trampoline has never classified vector arguments or
  -- results, and this fork does not add that. It must fail with a clear
  -- error, not silently pass the wrong register.
  local ok, err = pcall(ffi.cast, "simdt_cb2", function(a, b) return a end)
  check(not ok, "callback with a vector argument must be rejected")
  check(tostring(err):find("convert", 1, true), "clear error: " .. tostring(err))
end)

test("C calls with vectors inside a hot loop", function()
  local f4 = T.T.float4
  local a, b = f4.ct(1, 2, 3, 4), f4.ct(0.5)
  local jit_ = require("jit")
  if not pcall(jit_.on) then
    check(true, "JIT permanently disabled by a build option, skipped")
    return
  end
  jit_.off(); jit_.flush()
  local function run(n)
    local acc = f4.ct(0)
    for _ = 1, n do acc = C.simdt_addf4(acc, a) + b end
    return acc
  end
  local ref = run(300)
  jit_.on()
  local got
  for _ = 1, 4 do got = run(300) end
  jit_.off()
  checkeq(got, ref, "compiled C call with vectors")
end)

os.remove(csrc)
os.remove(clib)

return T
