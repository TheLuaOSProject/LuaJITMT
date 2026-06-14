local th = require"threading"
local ffi = require"ffi"

local rounds = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_DUP_STACK_ROUNDS")) or 30
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_DUP_STACK_ITERS")) or 200
local nthreads = 2

collectgarbage("stop")

local function run_case(round, kind, decl, cname, expected_size)
  ffi.cdef(decl)

  local ready = th.channel(nthreads)
  local start = th.channel(nthreads)
  local workers = {}

  for tid = 1, nthreads do
    workers[tid] = th.spawn(function(ready_ch, start_ch, id, count,
				      ctype_name, cdef_src, want_size)
      collectgarbage("stop")
      local ffi = require"ffi"
      local p01, p02, p03, p04 = id, count, ctype_name, cdef_src
      local p05, p06, p07, p08 = 5, 6, 7, 8
      local p09, p10, p11, p12 = 9, 10, 11, 12
      local p13, p14, p15, p16 = 13, 14, 15, 16
      local p17, p18, p19, p20 = 17, 18, 19, 20
      local p21, p22, p23, p24 = 21, 22, 23, 24
      local p25, p26, p27, p28 = 25, 26, 27, 28
      local p29, p30, p31, p32 = 29, 30, 31, 32
      local p33, p34, p35, p36 = 33, 34, 35, 36
      local p37, p38, p39, p40 = 37, 38, 39, 40
      local p41, p42, p43, p44 = 41, 42, 43, 44
      local p45, p46, p47, p48 = 45, 46, 47, 48
      local p49, p50, p51, p52 = 49, 50, 51, 52
      local p53, p54, p55, p56 = 53, 54, 55, 56
      local p57, p58, p59, p60 = 57, 58, 59, 60
      local p61, p62, p63, p64 = 61, 62, 63, 64
      local guard = p01 + p02 + p05 + p16 + p32 + p48 + p64
      if p03 ~= ctype_name or p04 ~= cdef_src or p63 ~= 63 then
	error("bad worker frame")
      end

      ready_ch:send(id)
      local token, ok = start_ch:recv(10)
      assert(ok == true and token == "go")

      for i = 1, count do
	ffi.cdef(cdef_src)
	local sz = ffi.sizeof(ctype_name)
	if sz ~= want_size then
	  error(("bad sizeof tid=%d i=%d got=%s want=%d"):format(
	    id, i, tostring(sz), want_size))
	end
	local ct = ffi.typeof(ctype_name)
	if ct == nil then
	  error(("bad typeof tid=%d i=%d"):format(id, i))
	end
      end

      return true, guard
    end, ready, start, tid, iters, cname, decl, expected_size)
  end

  for i = 1, nthreads do
    local _, ok = ready:recv(10)
    assert(ok == true, "ready timeout in " .. kind .. " round " .. round)
  end
  for _ = 1, nthreads do
    assert(start:send("go", 10) == true)
  end
  for tid = 1, nthreads do
    local ok, result = workers[tid]:join(30)
    assert(ok == true, tostring(result))
    assert(result == true)
  end
end

for round = 1, rounds do
  do
    local cname = ("lj_m7_dup_stack_int_%d_t"):format(round)
    run_case(round, "int", ("typedef int %s;"):format(cname), cname, 4)
  end
  do
    local cname = ("lj_m7_dup_stack_struct_%d_t"):format(round)
    run_case(round, "struct",
	     ("typedef struct { int x; double y; } %s;"):format(cname),
	     cname, 16)
  end
end

collectgarbage("restart")
collectgarbage("collect")

print(("t-ffi-cdef-dup-stack OK: %d rounds x %d iterations"):format(
  rounds, iters))
