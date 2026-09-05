local threading = require("threading")
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local t={}
local mts={}
for i=1,80 do mts[i]={} end
assert(threading.gcworkers(1) == 0)
jit.on()
for i=1,64 do setmetatable(t, mts[i]) end
assert(getmetatable(t) == mts[64])
assert(util.traceinfo(1), "setmetatable loop did not trace")
assert(threading.gcworkers(0) == 1)
