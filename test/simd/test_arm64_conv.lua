-- Scalar byte/halfword widening used by SIMD scalar references and FFI code.
-- In particular, U8/U16 XLOAD -> CONV U64 must not hit CONV EXT's assertion,
-- and negative signed inputs must keep their sign in the upper 32 bits.
local T = require("simdtest")
local ffi, test, checkeq = T.ffi, T.test, T.checkeq

if require("jit").arch ~= "arm64" then return T end

local cases = {
  {"int8_t", {-128, -1, 0, 127}},
  {"uint8_t", {0, 1, 128, 255}},
  {"int16_t", {-32768, -1, 0, 32767}},
  {"uint16_t", {0, 1, 32768, 65535}},
}

for _, case in ipairs(cases) do
  for _, dest in ipairs({"int64_t", "uint64_t"}) do
    local source, values = case[1], case[2]
    test(source.." to "..dest, function()
      local a = ffi.new(source.."[4]", values)
      local out = ffi.new(dest.."[4]")
      local divisor = ffi.new(dest, 2)
      local scalar = ffi.typeof(source)
      local target = ffi.typeof(dest)
      -- Keep division after the cast: it consumes the complete 64-bit value,
      -- rather than permitting a store to hide an incorrect upper half.
      local function run()
        for i = 1, 256 do
          local ix = i % 4
          out[ix] = ffi.cast(target, ffi.cast(scalar, a[ix])) / divisor
        end
      end
      run()
      for i = 1, 4 do
        local x = values[i]
        local expect
        if dest == "uint64_t" and x < 0 then
          -- Construct the exact modulo-2^64 reference outside the hot loop.
          expect = (0xffffffffffffffffULL - (-x-1)) / 2ULL
        else
          local q = x < 0 and math.ceil(x/2) or math.floor(x/2)
          expect = ffi.new(dest, q)
        end
        checkeq(out[i-1], expect, source.." value "..x.." -> "..dest)
      end
    end)
  end
end

return T
