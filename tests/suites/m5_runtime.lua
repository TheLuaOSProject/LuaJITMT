local runtime = require("suite_runtime")

local build_and_run_luajit_code = runtime.build_and_run_luajit_code
local build_and_run_luajit_script = runtime.build_and_run_luajit_script
local luajit_script = runtime.luajit_script

local function ctype_name_smoke()
  return [=[
local ffi = require"ffi"
for i = 1, 40 do
  ffi.cdef(([[typedef struct { int a; double b; } lj_ctype_name_s_%d;
typedef enum { LJ_CTYPE_NAME_E_%d = %d } lj_ctype_name_e_%d;]]):format(i, i, i, i))
  local ct = ffi.typeof(("lj_ctype_name_s_%d"):format(i))
  local x = ct(i, i + 0.5)
  assert(x.a == i and x.b == i + 0.5)
  local et = ffi.typeof(("lj_ctype_name_e_%d"):format(i))
  assert(tonumber(et(i)) == i)
  collectgarbage("collect")
end
local mt = ffi.metatype("struct { int x; }", {
  __index = { value = function(self) return self.x end }
})
assert(mt(7):value() == 7)
print("ctype-name-publish-smoke OK")
]=]
end

local function jit_hash_store_smoke()
  return [[
local trace_count = require("jit_harness").trace_count

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(trace_count(200) > 0, "existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(trace_count(200) > 0, "existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hn = {}
for i = 1, 200 do
  hn["k" .. i] = i
end
assert(hn.k200 == 200)
assert(trace_count(200) > 0, "new string hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local an = {}
    an[1] = i
    out = an
  end
  return out
end
local an = array_insert(80)
assert(an[1] == 80)
assert(trace_count(200) > 0, "fresh array slot table store did not trace")
]]
end

local function jit_table_fload_mutable_smoke()
  return [[
jit.opt.start("hotloop=1")
local sum = 0
local t = {}
for i = 1, 80 do t[i] = i end
for r = 1, 200 do
  if r == 75 then
    for i = 81, 180 do t[i] = i end
  end
  sum = sum + (t[(r % 180) + 1] or 0)
end
assert(sum > 0)
]]
end

local function jit_href_node_order_smoke()
  return [[
jit.opt.start("hotloop=1")
local t, keys = {}, {}
for i = 1, 128 do
  local k = "dyn" .. i
  keys[i] = k
  t[k] = i
end
local sum = 0
for i = 1, 800 do
  local k = keys[(i % 128) + 1]
  sum = sum + (t[k] or 0)
end
assert(sum > 0)
]]
end

local function jit_hrefk_record_snapshot_smoke()
  return [[
jit.opt.start("hotloop=1")
local t = { stable_key = 17, other = 23 }
local sum = 0
for i = 1, 800 do
  sum = sum + t.stable_key
end
assert(sum == 800 * 17)
]]
end

local function udtype_publish_smoke()
  return [[
local ffi = require"ffi"
ffi.cdef"int puts(const char *);"
assert(type(ffi.C.puts) == "cdata")

local f = io.tmpfile()
assert(io.type(f) == "file")
f:close()
assert(io.type(f) == "closed file")

local ok, buffer = pcall(require, "string.buffer")
if ok then
  for i = 1, 32 do
    local b = buffer.new(i % 8)
    collectgarbage("collect")
    assert(type(b) == "userdata")
  end
end

local th = require"threading"
local m = th.mutex()
assert(m:trylock() == true)
assert(m:trylock() == false)
assert(m:unlock() == nil)
local ch = th.channel(2)
assert(ch:send("x") == true)
local v, okrecv = ch:recv()
assert(v == "x" and okrecv == true)
local me = th.current()
assert(type(me:id()) == "number")
collectgarbage("collect")
collectgarbage("collect")
print("udtype-publish-smoke OK")
]]
end

return function(add)
  add({
    name = "m5_buffer_publish",
    description = "string.buffer publication thread smoke",
    run = function(t)
      t:build({ quiet = true })
      luajit_script(t, "t-buffer-thread-safety.lua", nil, { joff = true })
      luajit_script(t, "t-buffer-thread-safety.lua", nil, { jon = true })
      print("M5 string.buffer publication smoke passed")
    end
  })

  add({
    name = "m5_ctype_name_publish",
    description = "CType.name publication smoke test",
    run = function(t)
      build_and_run_luajit_code(t, ctype_name_smoke(), {
        clean = false,
        joff = true
      })
      print("M5 CType.name publication smoke passed")
    end
  })

  add({
    name = "m5_jit_hash_store_nyi",
    description = "JIT table-store bridge smoke",
    run = function(t)
      build_and_run_luajit_code(t, jit_hash_store_smoke())
      print("M5 JIT table-store bridge smoke passed")
    end
  })

  add({
    name = "m5_jit_table_fload_mutable",
    description = "JIT table field FLOAD mutability smoke",
    run = function(t)
      build_and_run_luajit_code(t, jit_table_fload_mutable_smoke())
      print("M5 JIT table FLOAD mutability smoke passed")
    end
  })

  add({
    name = "m5_jit_href_node_order",
    description = "x64 JIT HREF table node/hmask load ordering smoke",
    run = function(t)
      build_and_run_luajit_code(t, jit_href_node_order_smoke())
      print("M5 JIT HREF node-header hmask smoke passed")
    end
  })

  add({
    name = "m5_jit_hrefk_record_snapshot",
    description = "JIT HREFK recorder table shape snapshot smoke",
    run = function(t)
      build_and_run_luajit_code(t, jit_hrefk_record_snapshot_smoke())
      print("M5 JIT HREFK recorder snapshot smoke passed")
    end
  })

  add({
    name = "m5_udtype_publish",
    description = "userdata type publication smoke",
    run = function(t)
      build_and_run_luajit_code(t, udtype_publish_smoke(), {
        clean = false,
        joff = true
      })
      print("M5 userdata type publication smoke passed")
    end
  })

  add({
    name = "m5_threading_alloc",
    description = "per-TG allocator routing under spawned Lua threads",
    run = function(t)
      build_and_run_luajit_script(t, "t-threading-alloc.lua",
        { "4", "6000" }, { joff = true, timeout = "20s" })
      print("M5 threading allocator routing tests passed")
    end
  })

  add({
    name = "m5_math_random_tg",
    description = "per-TG math.random regression test",
    run = function(t)
      build_and_run_luajit_script(t, "t-math-random-tg.lua", nil,
        { joff = true })
    end
  })

  add({
    name = "m5_os_reentrant",
    description = "POSIX os.date/tmpname reentrancy and setlocale behavior",
    run = function(t)
      build_and_run_luajit_script(t, "t-os-reentrant.lua", nil, {
        joff = true,
        env = {
          LJ_M5_OS_THREADS = os.getenv("LJ_M5_OS_THREADS") or "8",
          LJ_M5_OS_ITERS = os.getenv("LJ_M5_OS_ITERS") or "200"
        }
      })
    end
  })

  add({
    name = "m5_libc_error_reentrant",
    description = "reentrant libc error-string formatting behavior",
    run = function(t)
      t:run([==[
if hits=$(grep -RInE -- 'strerror[[:space:]]*\(' src/*.c src/*.h | \
    grep -v '^src/lj_err.c:' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw production strerror() use is forbidden; use lj_err_strerrno()' >&2
  exit 1
fi
]==], { cwd = t.root, quiet = true })
      build_and_run_luajit_script(t, "t-libc-error-reentrant.lua", nil, {
        joff = true,
        env = {
          LJ_M5_ERRNO_THREADS = os.getenv("LJ_M5_ERRNO_THREADS") or "6",
          LJ_M5_ERRNO_ITERS = os.getenv("LJ_M5_ERRNO_ITERS") or "160"
        }
      })
    end
  })

  add({
    name = "m5_parser_capture_meta",
    description = "parser captured-local metadata behavior",
    run = function(t)
      build_and_run_luajit_script(t, "t-parser-capture-meta.lua", nil,
        { xcflags = "-DLUA_USE_ASSERT" })
    end
  })
end
