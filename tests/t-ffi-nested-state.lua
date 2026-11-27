local ffi = require("ffi")

ffi.cdef[[
typedef struct lua_State lua_State;
typedef double lua_Number;
lua_State *luaL_newstate(void);
void luaL_openlibs(lua_State *L);
void lua_close(lua_State *L);
int luaL_loadstring(lua_State *L, const char *s);
int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc);
lua_Number lua_tonumber(lua_State *L, int idx);
const char *lua_tolstring(lua_State *L, int idx, size_t *len);
void lua_pushlightuserdata(lua_State *L, void *p);
]]

local C = ffi.C

local function close_empty_state()
  local L = assert(C.luaL_newstate())
  C.lua_close(L)
end

local function run_nested_chunk()
  local L = assert(C.luaL_newstate())
  local src = "local x = 0; for i = 1, 100 do x = x + i end; return x"
  C.luaL_openlibs(L)
  assert(C.luaL_loadstring(L, src) == 0)
  assert(C.lua_pcall(L, 0, 1, 0) == 0)
  assert(C.lua_tonumber(L, -1) == 5050)
  C.lua_close(L)
end

local function run_nested_callbacks()
  local inner = assert(C.luaL_newstate())
  C.luaL_openlibs(inner)
  local chunk = [[
    local ffi = require("ffi")
    ffi.cdef("typedef int (*compare_t)(const void *, const void *);" ..
             "void qsort(void *, size_t, size_t, compare_t);")
    local outer_compare = ffi.cast("compare_t", ...)
    local calls, token = 0, {}
    local function exercise()
      local thread = coroutine.running()
      local cb = ffi.cast("int (*)(int)", function(x)
        assert(coroutine.running() == thread)
        calls = calls + 1
        if x < 0 then error(token) end
        ffi.errno(37)
        return x + 9
      end)
      assert(cb(5) == 14 and ffi.errno() == 37)
      local ok, err = pcall(cb, -1)
      assert(not ok and err == token)
      collectgarbage()
      assert(cb(24) == 33)
      cb:free()
      local compare = ffi.cast("compare_t", function(a, b)
        assert(coroutine.running() == thread)
        return outer_compare(a, b)
      end)
      local items = ffi.new("int[4]", {8, 3, 9, 1})
      ffi.C.qsort(items, 4, ffi.sizeof("int"), compare)
      assert(items[0] == 1 and items[1] == 3 and items[2] == 8 and items[3] == 9)
      compare:free()
    end
    exercise()
    coroutine.wrap(exercise)()
    return calls
  ]]
  local outer_calls, comparisons = 0, 0
  local compare = ffi.cast("int (*)(const void *, const void *)", function(a, b)
    comparisons = comparisons + 1
    return ffi.cast("const int *", a)[0] - ffi.cast("const int *", b)[0]
  end)
  local outer = ffi.cast("int (*)(int)", function(x)
    outer_calls = outer_calls + 1
    local rc = C.luaL_loadstring(inner, chunk)
    if rc == 0 then
      C.lua_pushlightuserdata(inner, ffi.cast("void *", compare))
      rc = C.lua_pcall(inner, 1, 1, 0)
    end
    if rc ~= 0 then error(ffi.string(C.lua_tolstring(inner, -1, nil))) end
    return x + C.lua_tonumber(inner, -1)
  end)
  assert(outer(36) == 42 and outer_calls == 1 and comparisons > 0)
  compare:free()
  outer:free()
  C.lua_close(inner)
end

for _ = 1, 3 do close_empty_state() end
run_nested_chunk()
run_nested_callbacks()
close_empty_state()

print("t-ffi-nested-state OK")
