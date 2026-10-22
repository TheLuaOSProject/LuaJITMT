/* ARM64 aggregate ABI and snapshot-wait regression. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_ccall.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"
#include "lib/test_sleep.h"

#if !LJ_TARGET_ARM64 || !defined(LJ_CCALL_TEST_HELPERS)
#error "requires ARM64 and LJ_CCALL_TEST_HELPERS"
#endif

static CType parent_snapshot;

typedef struct ParseWait {
  CTState *cts;
  TGState *tg;
  uint32_t seq;
  int saw_native;
} ParseWait;

static void *release_parser(void *arg)
{
  ParseWait *wait = (ParseWait *)arg;
  unsigned int i;
  for (i = 0; i < 2000; i++) {
    if (lj_tg_in_native_acq(wait->tg)) {
      wait->saw_native = 1;
      break;
    }
    sleep_ns(1000000);
  }
  ctype_parse_token_rel(wait->cts, wait->seq + 2u);
  (void)ctype_parse_token_wake(wait->cts, 0x7fffffff);
  return NULL;
}

static int classify_snapshot(lua_State *L)
{
  lua_pushinteger(L, lj_ccall_test_classify_struct(L, &parent_snapshot));
  return 1;
}

static void check_wait(lua_State *L, const char *name, unsigned int expected)
{
  CTState *cts = ctype_ctsG(G(L));
  CTypeID id;
  ParseWait wait;
  pthread_t thread;

  lua_getglobal(L, name);
  assert(tviscdata(L->top-1));
  id = *(CTypeID *)cdataptr(cdataV(L->top-1));
  id = ctype_rawid(cts, id);
  assert(lj_ctype_snapshot(cts, id, &parent_snapshot) > 0);
  lua_pop(L, 1);
  lua_pushcfunction(L, classify_snapshot);
  /* The parent is already a local snapshot. The classifier itself must wait
  ** when it follows the first field, without holding a runtime lock. */
  wait.cts = cts;
  wait.tg = L2TG(L);
  wait.seq = ctype_parse_token_acq(cts);
  wait.saw_native = 0;
  assert(!(wait.seq & 1u));
  ctype_parse_token_rel(cts, wait.seq + 1u);
  assert(pthread_create(&thread, NULL, release_parser, &wait) == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(wait.saw_native);
  assert(ctype_parse_token_acq(cts) == wait.seq + 2u);
  assert((unsigned int)lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  ljt_lua_dostring(L,
    "jit.off()\n"
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef struct { float v[2][2]; } Float4;\n"
    "typedef struct { double a, b; } Double2;\n"
    "typedef struct { Double2 inner; double c; } Double3;\n"
    "typedef union { double a; double b[2]; } DoubleUnion;\n"
    "typedef struct { double a; int b; } Mixed;\n"
    "typedef struct { int bits:3; float f; } Bits;\n"
    "typedef struct { long long a, b, c; } Large;\n"
    "typedef struct { double _Complex z; } Complex;\n"
    "Float4 arm64_float4(Float4);\n"
    "Double3 arm64_double3(Double3);\n"
    "DoubleUnion arm64_union(DoubleUnion);\n"
    "Mixed arm64_mixed(Mixed);\n"
    "Large arm64_large(Large);\n"
    "Complex arm64_complex(Complex);\n"
    "double arm64_fp_overflow(double,double,double,double,double,double,\n"
    "                         double,Double2,double);\n"
    "double arm64_gp_overflow(int,int,int,int,int,int,int,Mixed,int);\n"
    "]]\n"
    "for _, name in ipairs{'Float4','Double3','DoubleUnion','Mixed',\n"
    "                       'Bits','Large','Complex'} do\n"
    "  _G[name] = ffi.typeof(name)\n"
    "end\n"
    "local lib = ffi.load(assert(os.getenv('LJ_ARM64_AGGREGATE_LIB')))\n"
    "function run_aggregate_abi()\n"
    "  for i = 1, 200 do\n"
    "    local f = lib.arm64_float4(Float4({{{1,2},{3,4}}}))\n"
    "    assert(f.v[0][0]==2 and f.v[0][1]==4 and f.v[1][0]==6 and f.v[1][1]==8)\n"
    "    local d = lib.arm64_double3(Double3({{1,2},3}))\n"
    "    assert(d.inner.a==2 and d.inner.b==4 and d.c==6)\n"
    "    local u = DoubleUnion(); u.b[0]=1; u.b[1]=2\n"
    "    u = lib.arm64_union(u); assert(u.b[0]==2 and u.b[1]==4)\n"
    "    local m = lib.arm64_mixed(Mixed(1.25, 9))\n"
    "    assert(m.a==1.75 and m.b==16)\n"
    "    local l = lib.arm64_large(Large(1,2,3))\n"
    "    assert(l.a==11 and l.b==22 and l.c==33 and ffi.errno()==73)\n"
    "    local z = Complex(); z.z=ffi.new('complex',3,4); z=lib.arm64_complex(z)\n"
    "    assert(z.z.re==4 and z.z.im==6)\n"
    "    assert(lib.arm64_fp_overflow(1,2,3,4,5,6,7,ffi.new('Double2',8,9),10)==385)\n"
    "    assert(lib.arm64_gp_overflow(1,2,3,4,5,6,7,Mixed(8,9),10)==385)\n"
    "    if i % 25 == 0 then collectgarbage('collect') end\n"
    "  end\n"
    "end\n"
    "run_aggregate_abi()\n");

  check_wait(L, "Float4", 4 + (4 << 8));
  check_wait(L, "Double3", 8 + (3 << 8));
  check_wait(L, "DoubleUnion", 8 + (2 << 8));
  check_wait(L, "Mixed", 1);
  check_wait(L, "Bits", 1);
  check_wait(L, "Large", 0);
  check_wait(L, "Complex", 8 + (2 << 8));

  ljt_lua_dostring(L,
    "local threading = require('threading')\n"
    "local start = threading.channel(4)\n"
    "local workers = {}\n"
    "for id = 1, 4 do\n"
    "  workers[id] = assert(threading.spawn(function(ch, role)\n"
    "    local token, ok = ch:recv(10); assert(ok and token==1)\n"
    "    if role == 4 then\n"
    "      local ffi = require('ffi')\n"
    "      for i = 1, 512 do\n"
    "        ffi.cdef('typedef struct { double a; int b; } agg_growth_'..i..';')\n"
    "      end\n"
    "    else\n"
    "      run_aggregate_abi()\n"
    "    end\n"
    "    return true\n"
    "  end, start, id))\n"
    "end\n"
    "for i = 1, 4 do start:send(1) end\n"
    "for i = 1, 4 do\n"
    "  local ok, result = workers[i]:join(20); assert(ok and result==true)\n"
    "end\n");
  lua_close(L);
  puts("t-arm64-ffi-aggregate OK: HFA, nested arrays, unions, complex, mixed, sret, register overflow and snapshot waits");
  return 0;
}
