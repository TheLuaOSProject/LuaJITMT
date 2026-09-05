-- Stopped-GC diagnostic only: the cap is a pacing policy, not a chain bound.
local n=20000
local keep={}
collectgarbage("collect")
collectgarbage("stop")
gcdiag("stopped_before",0)
for i=1,n do keep[i]={i} end
gcdiag("stopped_after",0)
collectgarbage("collect")
gcdiag("collected",0)
for i=1,n do assert(keep[i][1]==i) end
