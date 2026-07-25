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
typedef float	 float2  __attribute__((vector_size(8)));

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

/* Callbacks taking and returning a vector by value, one per lane kind. */
typedef float4 (*simdt_cb2)(float4, float4);
float4 simdt_callcb(simdt_cb2 cb, float4 a, float4 b) { return cb(a, b); }
typedef double2 (*simdt_cbd2)(double2, double2);
double2 simdt_callcbd2(simdt_cbd2 cb, double2 a, double2 b) { return cb(a, b); }
typedef i32x4 (*simdt_cbi4)(i32x4, i32x4);
i32x4 simdt_callcbi4(simdt_cbi4 cb, i32x4 a, i32x4 b) { return cb(a, b); }
typedef i8x16 (*simdt_cbi8)(i8x16, i8x16);
i8x16 simdt_callcbi8(simdt_cbi8 cb, i8x16 a, i8x16 b) { return cb(a, b); }
/* An 8 byte vector uses only the low half of its register. */
typedef float2 (*simdt_cbf2)(float2, float2);
float2 simdt_callcbf2(simdt_cbf2 cb, float2 a, float2 b) { return cb(a, b); }

/* Ten vector arguments: eight in XMM registers, the last two on the stack. */
typedef float4 (*simdt_cb10)(float4, float4, float4, float4, float4,
			     float4, float4, float4, float4, float4);
float4 simdt_callcb10(simdt_cb10 cb, const float4 *v)
{
  return cb(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
}

/* The eight vectors use up every XMM register, so the doubles go on the stack
** first and each following vector has to be realigned to 16 bytes there.
*/
typedef float4 (*simdt_cbal)(float4, float4, float4, float4,
			     float4, float4, float4, float4,
			     double, float4, double, float4);
float4 simdt_callcbal(simdt_cbal cb, const float4 *v, double x, float4 p,
		      double y, float4 q)
{
  return cb(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], x, p, y, q);
}

/* Mixed integer, floating-point and vector arguments in a callback. */
typedef float4 (*simdt_cbmix)(int, float4, double, float4, long long, float4,
			      float);
float4 simdt_callcbmix(simdt_cbmix cb, int n, float4 a, double x, float4 b,
		       long long m, float4 c, float y)
{
  return cb(n, a, x, b, m, c, y);
}
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
typedef double2 (*simdt_cbd2)(double2, double2);
double2 simdt_callcbd2(simdt_cbd2 cb, double2 a, double2 b);
typedef i32x4 (*simdt_cbi4)(i32x4, i32x4);
i32x4   simdt_callcbi4(simdt_cbi4 cb, i32x4 a, i32x4 b);
typedef i8x16 (*simdt_cbi8)(i8x16, i8x16);
i8x16   simdt_callcbi8(simdt_cbi8 cb, i8x16 a, i8x16 b);
typedef float2 (*simdt_cbf2)(float2, float2);
float2  simdt_callcbf2(simdt_cbf2 cb, float2 a, float2 b);
typedef float4 (*simdt_cb10)(float4, float4, float4, float4, float4,
			     float4, float4, float4, float4, float4);
float4  simdt_callcb10(simdt_cb10 cb, const float4 *v);
typedef float4 (*simdt_cbal)(float4, float4, float4, float4,
			     float4, float4, float4, float4,
			     double, float4, double, float4);
float4  simdt_callcbal(simdt_cbal cb, const float4 *v, double x, float4 p,
		       double y, float4 q);
typedef float4 (*simdt_cbmix)(int, float4, double, float4, long long, float4,
			      float);
float4  simdt_callcbmix(simdt_cbmix cb, int n, float4 a, double x, float4 b,
			long long m, float4 c, float y);
/* Wider than a register: must still be rejected. float8 comes from simdtest. */
typedef float8 (*simdt_cb8)(float8, float8);
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

test("callbacks pass and return vectors by value", function()
  -- Subtraction on purpose: it is not commutative, so a swapped or misread
  -- argument register cannot cancel out.
  local cases = {
    {"float4", "simdt_cb2", C.simdt_callcb},
    {"double2", "simdt_cbd2", C.simdt_callcbd2},
    {"i32x4", "simdt_cbi4", C.simdt_callcbi4},
    {"i8x16", "simdt_cbi8", C.simdt_callcbi8},
  }
  for _, c in ipairs(cases) do
    local ti = T.T[c[1]]
    local rnd = T.rng(SEED + 400 + ti.bits)
    local cb = ffi.cast(c[2], function(a, b) return a - b end)
    for _ = 1, 40 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      checkeq(c[3](cb, a, b), a - b, ti.name .. " callback by value")
    end
    cb:free()
  end
end)

test("an 8 byte vector callback uses only half a register", function()
  local f2 = ffi.typeof("float2")
  local cb = ffi.cast("simdt_cbf2", function(a, b) return a - b end)
  local rnd = T.rng(SEED + 5)
  for _ = 1, 20 do
    local a = f2(rnd() % 1000 - 500, rnd() % 1000 - 500)
    local b = f2(rnd() % 1000 - 500, rnd() % 1000 - 500)
    checkeq(C.simdt_callcbf2(cb, a, b), a - b, "float2 callback by value")
  end
  cb:free()
end)

test("a callback receives ten vector arguments", function()
  -- Eight arrive in xmm0-xmm7, the last two on the stack.
  local f4 = T.T.float4
  local rnd = T.rng(SEED + 6)
  local v = ffi.new(ffi.typeof("$[10]", f4.ct))
  for i = 0, 9 do v[i] = T.randfinite(f4, rnd) end
  local seen
  local cb = ffi.cast("simdt_cb10", function(...)
    seen = {...}
    local acc = f4.ct(0)
    -- Weighted so that a permutation of the arguments changes the result.
    for i = 1, 10 do acc = acc + seen[i] * f4.ct(i) end
    return acc
  end)
  local want = f4.ct(0)
  for i = 1, 10 do want = want + v[i-1] * f4.ct(i) end
  checkeq(C.simdt_callcb10(cb, v), want, "ten vector callback arguments")
  check(seen and #seen == 10, "callback saw 10 arguments")
  for i = 1, 10 do
    checkeq(seen[i], v[i-1], "callback argument " .. i)
  end
  cb:free()
end)

test("stack vector arguments in a callback are 16 byte aligned", function()
  -- The eight leading vectors exhaust the XMM registers, so the doubles are
  -- passed on the stack and each following vector must skip the 8 bytes of
  -- padding the caller inserted. Reading it unaligned by one slot returns
  -- half of one vector and half of the next.
  local f4 = T.T.float4
  local rnd = T.rng(SEED + 7)
  local v = ffi.new(ffi.typeof("$[8]", f4.ct))
  for i = 0, 7 do v[i] = T.randfinite(f4, rnd) end
  local p, q = T.randfinite(f4, rnd), T.randfinite(f4, rnd)
  local gx, gp, gy, gq
  local cb = ffi.cast("simdt_cbal", function(a, b, c, d, e, f, g, h, x, pp, y, qq)
    gx, gp, gy, gq = x, pp, y, qq
    return pp - qq
  end)
  checkeq(C.simdt_callcbal(cb, v, 0.25, p, 0.75, q), p - q, "aligned stack vectors")
  checkeq(gx, 0.25, "stack double before a vector")
  checkeq(gy, 0.75, "stack double between two vectors")
  checkeq(gp, p, "first stack vector")
  checkeq(gq, q, "second stack vector")
  cb:free()
end)

test("mixed argument classes in a callback", function()
  local f4 = T.T.float4.ct
  local a, b, c = f4(1, 2, 3, 4), f4(10, 20, 30, 40), f4(100, 200, 300, 400)
  local got = {}
  local cb = ffi.cast("simdt_cbmix", function(n, va, x, vb, m, vc, y)
    got = { n = n, x = x, m = m, y = y, a = va, b = vb, c = vc }
    return va + vb + vc
  end)
  checkeq(C.simdt_callcbmix(cb, 7, a, 0.5, b, 9LL, c, 2.5), a + b + c,
	  "mixed callback result")
  checkeq(got.n, 7, "int argument")
  checkeq(got.x, 0.5, "double argument")
  checkeq(tonumber(got.m), 9, "long long argument")
  checkeq(got.y, 2.5, "float argument")
  checkeq(got.a, a, "vector argument 1")
  checkeq(got.b, b, "vector argument 2")
  checkeq(got.c, c, "vector argument 3")
  cb:free()
end)

test("callbacks with a vector wider than a register are rejected", function()
  -- 32 byte vectors are not passed in a single XMM register, so the callback
  -- trampoline has nowhere to put them. It must fail with a clear error
  -- rather than silently pass the wrong register.
  local ok, err = pcall(ffi.cast, "simdt_cb8", function(a, b) return a end)
  check(not ok, "callback with a 32 byte vector must be rejected")
  check(tostring(err):find("convert", 1, true), "clear error: " .. tostring(err))
end)

test("a callback with vectors runs from a compiled trace", function()
  local f4 = T.T.float4
  local jit_ = require("jit")
  if not pcall(jit_.on) then
    check(true, "JIT permanently disabled by a build option, skipped")
    return
  end
  local cb = ffi.cast("simdt_cb2", function(a, b) return a - b end)
  local a, b = f4.ct(1, 2, 3, 4), f4.ct(0.25)
  jit_.off(); jit_.flush()
  local function run(n)
    local acc = f4.ct(0)
    for _ = 1, n do acc = C.simdt_callcb(cb, acc, b) + a end
    return acc
  end
  local ref = run(300)
  jit_.on()
  local got
  for _ = 1, 4 do got = run(300) end
  jit_.off()
  checkeq(got, ref, "compiled call into a vector callback")
  cb:free()
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
