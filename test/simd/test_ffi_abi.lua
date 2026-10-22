-- FFI call and callback ABI for vector values.
--
-- Builds a small C helper library at test time and compares its results with
-- the Lua ones. Skips cleanly if no C compiler is available.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

-- Keep parallel test configurations from deleting each other's helper.
local tmpbase = os.tmpname()
os.remove(tmpbase)
local csrc = tmpbase .. ".c"
local clib = tmpbase .. ".so"

local CSRC = [[
typedef float	 float4  __attribute__((vector_size(16)));
typedef double	 double2 __attribute__((vector_size(16)));
typedef int	 i32x4   __attribute__((vector_size(16)));
typedef signed char i8x16 __attribute__((vector_size(16)));
typedef float	 float2  __attribute__((vector_size(8)));

float4 simdt_addf4(float4 a, float4 b) { return a + b; }
float2 simdt_addf2(float2 a, float2 b) { return a + b; }
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

/* Scalar aggregates still use one FP register per member, even though each
** register save slot must now preserve all 128 bits for vector arguments.
*/
typedef struct { float x[3]; } simdt_hfa3f;
typedef struct { double x[4]; } simdt_hfa4d;
simdt_hfa3f simdt_hfa3f_mix(float4 v, simdt_hfa3f a, double x, simdt_hfa3f b)
{
  simdt_hfa3f r;
  int i;
  for (i = 0; i < 3; i++) r.x[i] = a.x[i] + 2*b.x[i] + v[i] + x;
  return r;
}
simdt_hfa4d simdt_hfa4d_mix(simdt_hfa4d a, float4 v, simdt_hfa4d b, double x)
{
  simdt_hfa4d r;
  int i;
  for (i = 0; i < 4; i++) r.x[i] = a.x[i] + 3*b.x[i] + v[i] + x;
  return r;
}

typedef float _Complex simdt_cf;
typedef double _Complex simdt_cd;
simdt_cf simdt_complexf(float4 v, simdt_cf a, float x, simdt_cf b)
{
  simdt_cf r = a + 2*b;
  __real__ r += v[0] + x;
  __imag__ r += v[1];
  return r;
}
simdt_cd simdt_complexd(double a, double b, double c, double d,
		       double e, double f, double g, simdt_cd z, float4 v)
{
  __real__ z += a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + v[0];
  __imag__ z += v[1];
  return z;
}

/* AArch64 homogeneous short-vector aggregates (HVA). */
typedef struct { float4 x[2]; } simdt_hva2;
typedef struct { float2 x[3]; } simdt_hva3;
typedef struct __attribute__((aligned(16))) { float x; } simdt_pad16;
typedef struct __attribute__((aligned(32))) { float x; } simdt_pad32;
simdt_pad16 simdt_pad16_add(simdt_pad16 a, simdt_pad16 b)
{
  simdt_pad16 r = {a.x + b.x};
  return r;
}
simdt_pad32 simdt_pad32_add(simdt_pad32 a, simdt_pad32 b)
{
  simdt_pad32 r = {a.x + b.x};
  return r;
}
simdt_hva2 simdt_hva2_add(simdt_hva2 a, simdt_hva2 b)
{
  simdt_hva2 r = { {a.x[0] + b.x[0], a.x[1] + b.x[1]} };
  return r;
}
simdt_hva3 simdt_hva3_add(simdt_hva3 a, simdt_hva3 b)
{
  simdt_hva3 r = { {a.x[0] + b.x[0], a.x[1] + b.x[1], a.x[2] + b.x[2]} };
  return r;
}
simdt_hva2 simdt_hva2_spill(float4 a, float4 b, float4 c, float4 d,
			   float4 e, float4 f, float4 g, simdt_hva2 h, float4 i)
{
  h.x[0] += a + b + c + d;
  h.x[1] += e + f + g + i;
  return h;
}

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

/* Ten vector arguments: eight in SIMD registers, the last two on the stack. */
typedef float4 (*simdt_cb10)(float4, float4, float4, float4, float4,
			     float4, float4, float4, float4, float4);
float4 simdt_callcb10(simdt_cb10 cb, const float4 *v)
{
  return cb(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
}

/* The eight vectors use up every SIMD argument register, so doubles spill
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

/* Darwin packs spilled floats and small integers at their natural size.
** In particular, two spilled floats share one eight-byte stack slot.
*/
typedef float4 (*simdt_cbpack)(float4, float4, float4, float4,
			      float4, float4, float4, float4,
			      float, float, float4, float, float4);
float4 simdt_callcbpack(simdt_cbpack cb, const float4 *v, float4 p, float4 q)
{
  return cb(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
	    0.25f, 0.75f, p, 1.5f, q);
}
typedef float4 (*simdt_cbsmall)(int, int, int, int, int, int, int, int,
			       signed char, short, unsigned char, unsigned short);
float4 simdt_callcbsmall(simdt_cbsmall cb)
{
  return cb(1, 2, 3, 4, 5, 6, 7, 8, -101, -20003, 213, 60007);
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
float2  simdt_addf2(float2 a, float2 b);
double2 simdt_addd2(double2 a, double2 b);
i32x4   simdt_addi4(i32x4 a, i32x4 b);
i8x16   simdt_addi8(i8x16 a, i8x16 b);
float4  simdt_sum10(float4, float4, float4, float4, float4,
		    float4, float4, float4, float4, float4);
float4  simdt_mixed(int n, float4 a, double x, float4 b, long long m, float4 c);
float   simdt_lane(float4 a, int i);
void    simdt_store(float4 *p, float4 v);
float4  simdt_load(const float4 *p);
typedef struct { float x[3]; } simdt_hfa3f;
typedef struct { double x[4]; } simdt_hfa4d;
simdt_hfa3f simdt_hfa3f_mix(float4 v, simdt_hfa3f a, double x, simdt_hfa3f b);
simdt_hfa4d simdt_hfa4d_mix(simdt_hfa4d a, float4 v, simdt_hfa4d b, double x);
typedef complex float simdt_cf;
typedef complex double simdt_cd;
simdt_cf simdt_complexf(float4 v, simdt_cf a, float x, simdt_cf b);
simdt_cd simdt_complexd(double, double, double, double, double, double, double,
		       simdt_cd z, float4 v);
typedef struct { float4 x[2]; } simdt_hva2;
typedef struct { float2 x[3]; } simdt_hva3;
typedef struct __attribute__((aligned(16))) { float x; } simdt_pad16;
typedef struct __attribute__((aligned(32))) { float x; } simdt_pad32;
simdt_pad16 simdt_pad16_add(simdt_pad16 a, simdt_pad16 b);
simdt_pad32 simdt_pad32_add(simdt_pad32 a, simdt_pad32 b);
simdt_hva2 simdt_hva2_add(simdt_hva2 a, simdt_hva2 b);
simdt_hva3 simdt_hva3_add(simdt_hva3 a, simdt_hva3 b);
simdt_hva2 simdt_hva2_spill(float4, float4, float4, float4, float4, float4,
			   float4, simdt_hva2, float4);
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
typedef float4 (*simdt_cbpack)(float4, float4, float4, float4,
			      float4, float4, float4, float4,
			      float, float, float4, float, float4);
float4 simdt_callcbpack(simdt_cbpack cb, const float4 *v, float4 p, float4 q);
typedef float4 (*simdt_cbsmall)(int, int, int, int, int, int, int, int,
			       signed char, short, unsigned char, unsigned short);
float4 simdt_callcbsmall(simdt_cbsmall cb);
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

test("8 byte vector arguments and return", function()
  local f2 = ffi.typeof("float2")
  local a, b = f2(1.25, -2.5), f2(3.5, 9.75)
  checkeq(C.simdt_addf2(a, b), a + b, "float2 C add")
end)

test("float and double homogeneous aggregates retain their scalar ABI", function()
  local v = T.T.float4.ct(2, 4, 8, 16)
  local a = ffi.new("simdt_hfa3f", {{1, 3, 5}})
  local b = ffi.new("simdt_hfa3f", {{7, 11, 13}})
  local r = C.simdt_hfa3f_mix(v, a, 0.5, b)
  for i = 0, 2 do
    checkeq(r.x[i], a.x[i] + 2*b.x[i] + v[i] + 0.5, "float HFA member " .. i)
  end
  local c = ffi.new("simdt_hfa4d", {{1, 3, 5, 7}})
  local d = ffi.new("simdt_hfa4d", {{11, 13, 17, 19}})
  -- The second HFA cannot fit completely, so it and the trailing double spill.
  local s = C.simdt_hfa4d_mix(c, v, d, 0.25)
  for i = 0, 3 do
    checkeq(s.x[i], c.x[i] + 3*d.x[i] + v[i] + 0.25, "double HFA member " .. i)
  end
end)

test("complex arguments and results coexist with vectors", function()
  local v = T.T.float4.ct(3, 7, 11, 13)
  local a, b = ffi.new("simdt_cf", 1.25, -2.5), ffi.new("simdt_cf", 4.5, 8.25)
  local r = C.simdt_complexf(v, a, 0.5, b)
  checkeq(r.re, 13.75, "complex float real result")
  checkeq(r.im, 21, "complex float imaginary result")
  local z = ffi.new("simdt_cd", 0.125, -0.25)
  local s = C.simdt_complexd(1, 2, 3, 4, 5, 6, 7, z, v)
  checkeq(s.re, 143.125, "complex double spilled argument and real result")
  checkeq(s.im, 6.75, "complex double imaginary result")
end)

if ffi.arch == "arm64" then
  test("padding prevents over-aligned scalar structs from becoming HFAs", function()
    local a, b = ffi.new("simdt_pad16", {1.25}), ffi.new("simdt_pad16", {2.5})
    checkeq(C.simdt_pad16_add(a, b).x, 3.75, "16 byte struct passed in GPRs")
    local c, d = ffi.new("simdt_pad32", {3.25}), ffi.new("simdt_pad32", {4.5})
    checkeq(C.simdt_pad32_add(c, d).x, 7.75, "32 byte struct passed by reference")
  end)

  test("homogeneous vector aggregates use one SIMD register per member", function()
    for _, c in ipairs({{"simdt_hva2", "float4", 2, C.simdt_hva2_add},
			 {"simdt_hva3", "float2", 3, C.simdt_hva3_add}}) do
      local a, b = ffi.new(c[1]), ffi.new(c[1])
      local vec = ffi.typeof(c[2])
      for i = 0, c[3]-1 do
	a.x[i], b.x[i] = vec(2*i + 1), vec(3*i + 5)
      end
      local r = c[4](a, b)
      for i = 0, c[3]-1 do
	checkeq(r.x[i], a.x[i] + b.x[i], c[1] .. " member " .. i)
      end
    end
    local v = T.T.float4.ct(1, 2, 3, 4)
    local h = ffi.new("simdt_hva2")
    h.x[0], h.x[1] = T.T.float4.ct(10, 20, 30, 40), T.T.float4.ct(50, 60, 70, 80)
    local r = C.simdt_hva2_spill(v, v, v, v, v, v, v, h, v)
    checkeq(r.x[0], h.x[0] + v*4, "spilled HVA first vector")
    checkeq(r.x[1], h.x[1] + v*4, "spilled HVA second vector")
  end)
end

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
  -- Eight arrive in SIMD registers, the last two on the stack.
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
  -- The eight leading vectors exhaust the SIMD registers, so the doubles are
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

test("callbacks preserve packed floats between stack vectors", function()
  local f4 = T.T.float4.ct
  local v = ffi.new("float4[8]")
  for i = 0, 7 do v[i] = f4(i + 1) end
  local p, q = f4(2, 3, 5, 7), f4(11, 13, 17, 19)
  local seen
  local cb = ffi.cast("simdt_cbpack", function(a, b, c, d, e, f, g, h, x, y, pp, z, qq)
    seen = {x, y, z, pp, qq}
    return pp - qq
  end)
  checkeq(C.simdt_callcbpack(cb, v, p, q), p - q, "packed callback result")
  checkeq(seen[1], 0.25, "first packed float")
  checkeq(seen[2], 0.75, "second packed float")
  checkeq(seen[3], 1.5, "float between stack vectors")
  checkeq(seen[4], p, "first aligned vector after packed floats")
  checkeq(seen[5], q, "second aligned vector after a float")
  cb:free()
end)

test("callbacks preserve packed small integer stack arguments", function()
  local f4 = T.T.float4.ct
  local seen
  local cb = ffi.cast("simdt_cbsmall", function(...)
    seen = {...}
    return f4(seen[9], seen[10], seen[11], seen[12])
  end)
  checkeq(C.simdt_callcbsmall(cb), f4(-101, -20003, 213, 60007), "packed integers")
  for i = 1, 8 do checkeq(seen[i], i, "leading integer " .. i) end
  cb:free()
end)

test("callbacks with a vector wider than a register are rejected", function()
  -- 32 byte vectors are not passed in a single SIMD register, so the callback
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
