-- Randomized program stress test.
--
-- Generates random vector expressions as Lua source, runs each one
-- interpreted and compiled, and compares bit for bit. The generated loop has
-- a rare branch so the exit becomes hot and grows a side trace, which
-- exercises snapshot replay of sunk vector boxes as well.
--
-- Every failure prints the generated source, so it can be replayed directly.
local T = require("simdtest")
local ffi, simd, test, check = T.ffi, T.simd, T.test, T.check
local jit_ = require("jit")

-- This file needs a working JIT compiler.
if not pcall(jit_.on) then
  test("JIT compiler", function()
    check(true, "JIT permanently disabled by a build option, stress skipped")
  end)
  return T
end

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")
local PROGRAMS = tonumber(os.getenv("SIMD_STRESS_PROGRAMS") or "120")

-- Operations that take a vector and give back the same ctype ---------------

-- Each entry: {arity, template, predicate(ti)}. "%1"/"%2" are subexpressions.
local unary = {
  {"-(%1)"},
  {"simd.bnot(%1)"},
  {"simd.abs(%1)"},
  {"simd.sqrt(%1)", function(ti) return ti.fp end},
  {"simd.floor(%1)", function(ti) return ti.fp end},
  {"simd.ceil(%1)", function(ti) return ti.fp end},
  {"simd.trunc(%1)", function(ti) return ti.fp end},
  {"simd.round(%1)", function(ti) return ti.fp end},
}

local binary = {
  {"(%1) + (%2)"},
  {"(%1) - (%2)"},
  {"(%1) * (%2)"},
  {"(%1) / (%2)", function(ti) return ti.fp end},
  {"simd.min(%1, %2)"},
  {"simd.max(%1, %2)"},
  {"simd.band(%1, %2)"},
  {"simd.bor(%1, %2)"},
  {"simd.bxor(%1, %2)"},
  {"simd.bandn(%1, %2)"},
  {"simd.adds(%1, %2)", function(ti) return not ti.fp and ti.bits <= 16 end},
  {"simd.subs(%1, %2)", function(ti) return not ti.fp and ti.bits <= 16 end},
}

local cmps = {"eq", "ne", "lt", "le", "gt", "ge"}

local function pick(rnd, list, ti)
  for _ = 1, 40 do
    local e = list[(rnd() % #list) + 1]
    if not e[2] or e[2](ti) then return e end
  end
  return list[1]
end

local function subst(tmpl, a, b)
  return (tmpl:gsub("%%1", function() return a end)
	       :gsub("%%2", function() return b end))
end

-- Build one expression of the vector's own ctype.
local function genexpr(rnd, ti, depth)
  if depth <= 0 then
    local vars = {"a", "b", "c", "acc"}
    return vars[(rnd() % #vars) + 1]
  end
  local r = rnd() % 100
  if r < 12 then                                  -- scalar operand
    local k = ti.fp and (rnd() % 7) + 0.5 or (rnd() % 5) + 1
    local op = ({"+", "-", "*"})[(rnd() % 3) + 1]
    return "(" .. genexpr(rnd, ti, depth-1) .. ") " .. op .. " " .. tostring(k)
  elseif r < 22 and not ti.fp then                -- shift by a constant
    local op = ({"shl", "shr", "sar"})[(rnd() % 3) + 1]
    return "simd." .. op .. "(" .. genexpr(rnd, ti, depth-1) .. ", " ..
	   tostring(rnd() % (ti.bits + 2)) .. ")"
  elseif r < 30 and not ti.fp then                -- shift by a variable
    local op = ({"shl", "shr", "sar"})[(rnd() % 3) + 1]
    return "simd." .. op .. "(" .. genexpr(rnd, ti, depth-1) .. ", i % " ..
	   tostring(ti.bits + 1) .. ")"
  elseif r < 38 then                              -- select over a comparison
    local c = cmps[(rnd() % #cmps) + 1]
    return "simd.select(simd." .. c .. "(" .. genexpr(rnd, ti, depth-1) ..
	   ", " .. genexpr(rnd, ti, depth-1) .. "), " ..
	   genexpr(rnd, ti, depth-1) .. ", " .. genexpr(rnd, ti, depth-1) .. ")"
  elseif r < 44 then                              -- lane insert
    return "simd.insert(" .. genexpr(rnd, ti, depth-1) .. ", " ..
	   ((rnd() % 2 == 0) and tostring(rnd() % ti.lanes) or
	    ("i % " .. tostring(ti.lanes))) .. ", " ..
	   tostring(ti.fp and 2.5 or 3) .. ")"
  elseif r < 50 then                              -- lane shuffle
    local idx = {}
    for j = 1, ti.lanes do idx[j] = tostring(rnd() % ti.lanes) end
    return "simd.shuffle(" .. genexpr(rnd, ti, depth-1) .. ", " ..
	   table.concat(idx, ", ") .. ")"
  elseif r < 56 then                              -- bitcast round trip
    local other = ti.bits == 8 and "u8x16" or ti.bits == 16 and "u16x8" or
		  ti.bits == 32 and "u32x4" or "u64x2"
    return "simd.bitcast(ct, simd.bnot(simd.bitcast(bc, " ..
	   genexpr(rnd, ti, depth-1) .. ")))"
  elseif r < 70 then                              -- unary
    return subst(pick(rnd, unary, ti)[1], genexpr(rnd, ti, depth-1), "")
  end
  local e = pick(rnd, binary, ti)                 -- binary
  return subst(e[1], genexpr(rnd, ti, depth-1), genexpr(rnd, ti, depth-1))
end

--[[
Something that reduces a vector to a number, for the rare branch.

For floating-point lanes only the queries that yield an *integer* are used.
Bitwise operations on a float vector can produce a NaN whose payload collides
with LuaJIT's GC64 type tags, and turning such a bit pattern into a Lua number
corrupts the VM. That is inherited LuaJIT behaviour, identical to reading a
double out of FFI memory, and is recorded in notes/SIMD_MATRIX.md; it is not
something this generator should be exercising.
]]
local function genquery(rnd, ti, expr)
  local r = rnd() % (ti.fp and 3 or 6)
  if r == 0 then return "simd.movemask(simd.lt(" .. expr .. ", b))"
  elseif r == 1 then return "(simd.anyof(simd.gt(" .. expr .. ", c)) and 1 or 0)"
  elseif r == 2 then return "(simd.allof(simd.eq(" .. expr .. ", " .. expr .. ")) and 1 or 0)"
  elseif r == 3 then return "tonumber(simd.hsum(" .. expr .. "))"
  elseif r == 4 then return "tonumber(simd.hmin(" .. expr .. "))"
  end
  return "tonumber((" .. expr .. ")[i % " .. tostring(ti.lanes) .. "])"
end

local function genprogram(rnd, ti)
  local e = genexpr(rnd, ti, 3)
  local q = genquery(rnd, ti, "t")
  local combine = ({"acc + t", "simd.bxor(acc, t)", "simd.max(acc, t)",
		    "simd.min(acc, t)"})[(rnd() % 4) + 1]
  return table.concat({
    "local simd, ct, bc = ...",
    "return function(a, b, c, n)",
    "  local acc = ct(0)",
    "  local s = 0",
    "  for i = 1, n do",
    "    local t = " .. e,
    "    if i % 53 == 0 then s = s + " .. q .. " end",
    "    acc = " .. combine,
    "  end",
    "  return acc, s",
    "end",
  }, "\n")
end

test("randomized vector programs agree between interpreter and JIT", function()
  local rnd = T.rng(SEED)
  local bccache = {}
  for prog = 1, PROGRAMS do
    local ti = T.T[(rnd() % #T.T) + 1]
    local bcname = ti.bits == 8 and "u8x16" or ti.bits == 16 and "u16x8" or
		   ti.bits == 32 and "u32x4" or "u64x2"
    local bc = bccache[bcname]
    if not bc then bc = ffi.typeof(bcname); bccache[bcname] = bc end
    local src = genprogram(rnd, ti)
    local chunk, err = loadstring(src, "=stress" .. prog)
    if not check(chunk, "program " .. prog .. " failed to load: " ..
		 tostring(err) .. "\n" .. src) then
      -- keep going with the next program
    else
      local a = T.rand(ti, rnd)
      local b = T.rand(ti, rnd)
      local c = T.rand(ti, rnd)
      if os.getenv("SIMD_STRESS_VERBOSE") then
	io.stderr:write("---- program ", prog, " (", ti.name, ")\na=",
			T.hexbytes(a), "\nb=", T.hexbytes(b), "\nc=",
			T.hexbytes(c),
			"\n", src, "\n")
	io.stderr:flush()
      end
      -- Two independent closures so each mode gets its own trace state.
      local fi = chunk(simd, ti.ct, bc)
      local fj = loadstring(src, "=stress" .. prog)(simd, ti.ct, bc)
      jit_.off(); jit_.flush()
      local ok1, r1, s1 = pcall(fi, a, b, c, 600)
      jit_.on()
      local ok2, r2, s2 = pcall(fj, a, b, c, 600)
      jit_.off()
      local ctx = string.format(
	"program %d (%s, seed %d)\na=%s\nb=%s\nc=%s\n%s",
	prog, ti.name, SEED, T.hexbytes(a), T.hexbytes(b), T.hexbytes(c), src)
      if check(ok1 == ok2, "raise/return disagreement: " .. ctx ..
	       "\ninterp: " .. tostring(r1) .. "\njit:    " .. tostring(r2)) then
	if ok1 then
	  check(T.same(r1, r2), "vector result differs: " .. ctx ..
		"\ninterp " .. T.hexbytes(r1) .. "\njit    " .. T.hexbytes(r2))
	  check(s1 == s2 or (s1 ~= s1 and s2 ~= s2),
		"side-exit sum differs: " .. ctx ..
		"\ninterp " .. tostring(s1) .. "\njit    " .. tostring(s2))
	end
      end
    end
  end
end)

return T
