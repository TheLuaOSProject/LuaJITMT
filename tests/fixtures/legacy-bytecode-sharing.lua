local threading=require('threading')
local x=10
local function get() return x end
local task=threading.spawn(function() x=22; return get() end)
local ok,value=task:join()
assert(ok and value==22 and x==22 and get()==22)
x=33
assert(get()==33)
return true
