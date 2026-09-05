local nkeys = tonumber(arg[1]) or 0
local keys, tab = {}, {}
for i=1,nkeys do keys[i] = "pacing"..i; tab[keys[i]] = i end
local function churn(n)
  local s=0
  for i=1,n do
    local x=i
    local f=function() x=x+1; return x end
    s=s+f()
  end
  return s
end
churn(300)
collectgarbage("collect")
collectgarbage("collect")
gcdiag("before",0)
for round=1,3 do
  assert(churn(50000)==50000*50003/2)
  gcdiag("after",round)
  collectgarbage("collect")
  collectgarbage("collect")
  gcdiag("settled",round)
end
if nkeys>0 then assert(tab[keys[nkeys]]==nkeys) end
