-- Decode x86-64 instruction bytes without executing them.
local T = require("simdtest")
package.path = "src/?.lua;" .. package.path
local dis = require("jit.dis_x64")

-- Test both VEX.W values in one context, including a two-byte VEX prefix
-- immediately after a three-byte prefix with W set.
local instructions = {
  {"\xc4\xc1\xf9\x6e\xc9", "vmovq xmm1, r9"},
  {"\xc4\xc1\x79\x6e\xc9", "vmovd xmm1, r9d"},
  {"\xc4\xc3\xe9\x22\xc9\x01", "vpinsrq xmm1, xmm2, r9, 0x01"},
  {"\xc4\xc3\x69\x22\xc9\x03", "vpinsrd xmm1, xmm2, r9d, 0x03"},
  {"\xc4\xe2\xe9\x47\xcb", "vpsllvq xmm1, xmm2, xmm3"},
  {"\xc4\xe2\x69\x47\xcb", "vpsllvd xmm1, xmm2, xmm3"},
  {"\xc4\xe2\x6d\x45\xcb", "vpsrlvd ymm1, ymm2, ymm3"},
  {"\xc4\xe2\xed\x45\xcb", "vpsrlvq ymm1, ymm2, ymm3"},
  {"\xc5\xe8\x58\xcb", "vaddps xmm1, xmm2, xmm3"},
  {"\xc4\xc1\x68\x58\xc9", "vaddps xmm1, xmm2, xmm9"},
  {"\xc4\xc1\x6c\x58\x49\x10", "vaddps ymm1, ymm2, [r9+0x10]"},
}

T.test("VEX prefix and operand widths", function()
  local bytes, decoded = {}, {}
  for i, instruction in ipairs(instructions) do bytes[i] = instruction[1] end
  local ctx = dis.create(table.concat(bytes), 0, function(line)
    decoded[#decoded+1] = line:match("^%x+%s+(.-)\n$") or line
  end)
  ctx.hexdump = 0
  ctx:disass()
  T.checkeq(#decoded, #instructions, "instruction count")
  for i, instruction in ipairs(instructions) do
    T.checkeq(decoded[i], instruction[2], "instruction " .. i)
  end
end)

return T
