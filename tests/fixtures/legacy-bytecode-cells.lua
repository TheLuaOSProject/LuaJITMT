local function identity(x)
  local function a() return x end
  local function b() return x end
  assert(debug.upvalueid(a,1)==debug.upvalueid(b,1))
  debug.setupvalue(a,1,21)
  assert(a()==21 and b()==21 and x==21)
  x=22
  assert(a()==22 and b()==22)
  return a,b
end
local a,b=identity(1)
debug.setupvalue(a,1,23)
assert(a()==23 and b()==23)
local numeric, generic, repeated, whiled = {},{},{},{}
for i=1,8 do
  numeric[i]=function() return i end
end
for k,v in ipairs({11,12,13,14,15,16,17,18}) do
  generic[k]=function() return k,v end
end
local keyed={}
for k,v in pairs({a=1,b=2,c=3}) do
  keyed[k]=function() return k,v end
end
for k,f in pairs(keyed) do
  local ck,cv=f()
  assert(ck==k and cv==({a=1,b=2,c=3})[k])
end
local i=0
repeat
  i=i+1
  local x=i
  repeated[i]=function(n) x=x+(n or 0);return x end
until i==8
local j=0
while j<8 do
  j=j+1
  local x=j
  whiled[j]=function()return x end
end
for k=1,8 do
  local gk,gv=generic[k]()
  assert(numeric[k]()==k and gk==k and gv==k+10)
  assert(repeated[k](10)==k+10 and repeated[k]()==k+10 and whiled[k]()==k)
end
local function conditional(v)
  local x=v
  local function get() return x end
  x=x or 10
  assert(get()==x)
  x=x and 20
  assert(get()==x)
  if v then x=21 else x=22 end
  return get(),x
end
assert(conditional(true)==21 and conditional(false)==22)
local function calls(x,...)
  local function get()return x end
  local function multi()return 3,4,5 end
  x=multi()
  assert(x==3 and get()==3)
  local y,z=...
  x,y,z=multi()
  assert(get()==3 and y==4 and z==5)
  return get(),y,z,...
end
local x,y,z,p,q=calls(1,7,8)
assert(x==3 and y==4 and z==5 and p==7 and q==8)
local function recursion(n)
  local function f(n) if n==0 then return 0 end return f(n-1)+1 end
  return f(n)
end
assert(recursion(20)==20)
-- A trace may promote x before the branch which skips its eventual FNEW.
local function branch(x, take)
  if take then
    local function get() return x end
    x=x+1
    return get()
  end
  return x
end
for i=1,160 do
  assert(branch(i, true)==i+1)
  assert(branch(i, false)==i)
end
local capture={answer=42}
local function gettable()return capture end
capture.answer=43
assert(gettable()==capture and gettable().answer==43)
return true
