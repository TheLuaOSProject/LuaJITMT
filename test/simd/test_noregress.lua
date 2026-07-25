-- Ordinary Lua and FFI behaviour that must be byte-for-byte identical to
-- upstream LuaJIT. This file uses no vector types at all.
local ffi = require("ffi")
local bit = require("bit")
local out = {}
local function p(...) local t={} for i=1,select('#',...) do t[i]=tostring((select(i,...))) end out[#out+1]=table.concat(t,"\t") end

-- numbers, strings, tables, closures, metatables, coroutines
local s=0 for i=1,200000 do s=s+i*0.5 end p("sum",s)
local t={} for i=1,1000 do t[i]=i*i end p("tab",#t,t[500])
p("str", ("%d %.14g %s"):format(42, 1/3, ("x"):rep(5)), ("abc"):upper(), ("a,b,c"):find(","))
p("bit", bit.band(0xf0f0,0xff), bit.bxor(-1,0x5a), bit.lshift(1,31), bit.tohex(255))
p("math", math.floor(-1.5), math.ceil(-1.5), math.fmod(7,3), math.max(1,2,3), 2^53)
local co=coroutine.wrap(function(a) for i=1,3 do a=coroutine.yield(a*2) end return a end)
p("co", co(3), co(4), co(5))
local mt={__index=function(_,k) return k.."!" end, __add=function(a,b) return 99 end}
local o=setmetatable({},mt) p("mt", o.foo, o+o)
p("pcall", pcall(function() error("boom") end))
p("sort", (function() local a={5,3,1,4,2} table.sort(a) return table.concat(a,",") end)())
-- ffi scalars, structs, arrays, pointers, callbacks
ffi.cdef[[ typedef struct { int a; double b; char c[8]; } S; ]]
local sv=ffi.new("S",{1,2.5,"hi"}) p("ffi", sv.a, sv.b, ffi.string(sv.c), ffi.sizeof("S"), ffi.alignof("S"))
local arr=ffi.new("int64_t[10]") for i=0,9 do arr[i]=i*1000000007LL end
local acc=0LL for i=0,9 do acc=acc+arr[i] end p("i64", tostring(acc), tostring(acc*3), tostring(-acc))
p("u64", tostring(ffi.cast("uint64_t",-1)), tostring(2ULL^63))
local cb=ffi.cast("int(*)(int,int)", function(a,b) return a*b+1 end)
p("cb", cb(6,7))
cb:free()
-- Nine doubles: eight arrive in FPRs, the ninth on the stack. This is the
-- shape that a change to the callback FPR save area would break first.
local cbd=ffi.cast("double(*)(double,double,double,double,double,double,double,double,double)",
  function(...) local t={...} local s=0 for i=1,9 do s=s+t[i]*i end return s end)
p("cbd", cbd(1.5,2.25,3.125,4.0625,5,6,7,8,9), cbd(1/3,-0.0,1e308,2^-1074,1,1,1,1,1))
cbd:free()
local cbf=ffi.cast("float(*)(float,int,double,int,float)",
  function(a,b,c,d,e) return a+b+c+d+e end)
p("cbf", cbf(1.5,2,3.25,4,5.75), cbf(1/3,1,1/7,1,1/9))
cbf:free()
p("cast", tonumber(ffi.cast("int8_t",300)), tonumber(ffi.cast("uint16_t",-1)), tonumber(ffi.cast("float",1/3)))
-- string.format / tostring of cdata types
p("types", tostring(ffi.typeof("S")), tostring(ffi.typeof("int (*)(void)")))
-- hot loops so the JIT is exercised
local function fib(n) if n<2 then return n end return fib(n-1)+fib(n-2) end p("fib", fib(24))
local z=0 for i=1,300000 do z = bit.bxor(z, i) + (i%7) end p("loop", z)
-- Written so it can be diffed against a pristine LuaJIT build:
--   luajit test/simd/test_noregress.lua > new.txt
--   (pristine)/luajit test/simd/test_noregress.lua > base.txt && diff base.txt new.txt
print(table.concat(out,"\n"))
