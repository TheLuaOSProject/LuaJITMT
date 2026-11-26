local buffer = require("string.buffer")
local first, second = {kind="first"}, {kind="second"}
local options = {dict={"alpha", "beta", "gamma"}, metatable={first,second}}
local enc, dec = buffer.new(options), buffer.new(options)
local value = setmetatable({alpha=17, beta=29, gamma="last"}, second)
local got = dec:set(enc:encode(value):get()):decode()
assert(got.alpha==17 and got.beta==29 and got.gamma=="last")
assert(getmetatable(got)==second and #dec==0)
print("nonzero buffer dictionary indexes OK")
