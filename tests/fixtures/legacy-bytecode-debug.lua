local function check(x)
  local y=10
  local function pair() return x,y end
  local nx,vx=debug.getlocal(1,1)
  local ny,vy=debug.getlocal(1,2)
  assert(nx=='x' and vx==2 and ny=='y' and vy==10)
  assert(debug.setlocal(1,1,20)=='x')
  assert(debug.setlocal(1,2,30)=='y')
  local a,b=pair()
  assert(x==20 and y==30 and a==20 and b==30)
  local ok,err=pcall(function() error('location') end)
  assert(not ok and err:match('stock%-cell%-debug.lua:11: location'))
  return x+y
end
assert(check(2)==50)
return true
