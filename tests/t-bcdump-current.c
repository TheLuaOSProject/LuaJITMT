/*
** Focused guard for lockless bytecode dump compatibility.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_bcdump.h"

#include "lib/lua_fixture_helpers.h"

typedef struct DumpBuf {
  char *p;
  size_t n;
  size_t cap;
} DumpBuf;

static int load_dump(lua_State *L, const DumpBuf *b);

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
  DumpBuf *b = (DumpBuf *)ud;
  UNUSED(L);
  if (sz > b->cap - b->n) {
    size_t cap = b->cap ? b->cap : 256;
    char *np;
    while (sz > cap - b->n)
      cap *= 2;
    np = (char *)realloc(b->p, cap);
    assert(np != NULL);
    b->p = np;
    b->cap = cap;
  }
  memcpy(b->p + b->n, p, sz);
  b->n += sz;
  return 0;
}

static void dump_reset(DumpBuf *b)
{
  b->n = 0;
}

static void dump_free(DumpBuf *b)
{
  free(b->p);
  b->p = NULL;
  b->n = b->cap = 0;
}

static void dump_copy(DumpBuf *dst, const DumpBuf *src)
{
  if (dst->cap < src->n) {
    char *np = (char *)realloc(dst->p, src->n);
    assert(np != NULL);
    dst->p = np;
    dst->cap = src->n;
  }
  memcpy(dst->p, src->p, src->n);
  dst->n = src->n;
}

static uint32_t read_uleb(const DumpBuf *b, size_t *ofs)
{
  uint32_t x = 0;
  uint32_t shift = 0;
  for (;;) {
    uint8_t c;
    assert(*ofs < b->n);
    c = (uint8_t)b->p[(*ofs)++];
    x |= (uint32_t)(c & 0x7f) << shift;
    if ((c & 0x80) == 0)
      return x;
    shift += 7;
    assert(shift < 32);
  }
}

static size_t first_proto_offset(const DumpBuf *b)
{
  size_t ofs = 4;
  uint32_t flags, plen;
  assert(b->n > 6);
  flags = read_uleb(b, &ofs);
  if ((flags & BCDUMP_F_STRIP) == 0) {
    uint32_t namelen = read_uleb(b, &ofs);
    assert(ofs + namelen <= b->n);
    ofs += namelen;
  }
  plen = read_uleb(b, &ofs);
  assert(plen != 0);
  assert(ofs + plen <= b->n);
  return ofs;
}

static size_t first_bc_offset(const DumpBuf *b, uint32_t *framesize,
			      uint32_t *numbc)
{
  size_t ofs = 4, pstart, pend;
  uint32_t flags, plen;
  assert(b->n > 6);
  assert((uint8_t)b->p[0] == BCDUMP_HEAD1);
  assert((uint8_t)b->p[1] == BCDUMP_HEAD2);
  assert((uint8_t)b->p[2] == BCDUMP_HEAD3);
  flags = read_uleb(b, &ofs);
  if ((flags & BCDUMP_F_STRIP) == 0) {
    uint32_t namelen = read_uleb(b, &ofs);
    assert(ofs + namelen <= b->n);
    ofs += namelen;
  }
  plen = read_uleb(b, &ofs);
  assert(plen != 0);
  pstart = ofs;
  pend = pstart + plen;
  assert(pend <= b->n);
  ofs++;  /* proto flags */
  ofs++;  /* numparams */
  assert(ofs < pend);
  *framesize = (uint8_t)b->p[ofs++];
  ofs++;  /* sizeuv */
  (void)read_uleb(b, &ofs);  /* sizekgc */
  (void)read_uleb(b, &ofs);  /* sizekn */
  *numbc = read_uleb(b, &ofs);
  assert(*numbc > 0);
  assert(ofs + (size_t)*numbc * sizeof(BCIns) <= pend);
  return ofs;
}

static void patch_ins(DumpBuf *b, size_t ofs, BCIns ins)
{
  assert(ofs + sizeof(ins) <= b->n);
  memcpy(b->p + ofs, &ins, sizeof(ins));
}

static int is_internal_jit_bc(BCOp op)
{
  return op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP ||
	 op == BC_JFORI || op == BC_JFORL || op == BC_JITERL ||
	 op == BC_JLOOP;
}

static void assert_no_internal_jit_bc(const DumpBuf *b)
{
  uint32_t framesize, numbc;
  size_t pos = first_bc_offset(b, &framesize, &numbc);
  uint32_t i;
  UNUSED(framesize);
  for (i = 0; i < numbc; i++, pos += sizeof(BCIns)) {
    BCIns ins;
    memcpy(&ins, b->p + pos, sizeof(ins));
    assert(!is_internal_jit_bc(bc_op(ins)));
  }
}

static GCproto *top_proto(lua_State *L)
{
  GCfunc *fn;
  assert(tvisfunc(L->top-1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  return funcproto(fn);
}

static void compile_to_dump(lua_State *L, const char *src, DumpBuf *b)
{
  dump_reset(b);
  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "luaL_loadstring");
  assert(lj_bcwrite(L, top_proto(L), dump_writer, b, BCDUMP_F_STRIP) == 0);
  assert(b->n > 5);
  assert((uint8_t)b->p[3] == BCDUMP_VERSION_LOCKLESS);
  lua_pop(L, 1);
}

static void assert_jit_patched_dump_unpatches(lua_State *L)
{
  DumpBuf dump = { NULL, 0, 0 };
  const char *src =
    "return function(n) local s=0 for i=1,n do s=s+i end return s end";
  int i;
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "jit.flush(); jit.opt.start('hotloop=1','hotexit=1')"),
    "enable hot JIT");
  ljt_lua_assert_ok(L, luaL_loadstring(L, src), "load JIT dump function");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "create JIT dump function");
  for (i = 0; i < 40; i++) {
    lua_pushvalue(L, -1);
    lua_pushinteger(L, 100);
    ljt_lua_assert_ok(L, lua_pcall(L, 1, 1, 0), "run JIT dump function");
    assert(lua_tointeger(L, -1) == 5050);
    lua_pop(L, 1);
  }
  assert(lua_dump(L, dump_writer, &dump) == 0);
  assert_no_internal_jit_bc(&dump);
  ljt_lua_assert_ok(L, load_dump(L, &dump), "load dumped JIT function");
  lua_pushinteger(L, 100);
  ljt_lua_assert_ok(L, lua_pcall(L, 1, 1, 0), "run dumped JIT function");
  assert(lua_tointeger(L, -1) == 5050);
  lua_pop(L, 2);
  dump_free(&dump);
}

static int load_dump(lua_State *L, const DumpBuf *b)
{
  return luaL_loadbufferx(L, b->p, b->n, "=(bcdump-compat)", "b");
}

static void assert_load_fails(lua_State *L, const DumpBuf *b, const char *what)
{
  int status = load_dump(L, b);
  if (status == LUA_OK) {
    fprintf(stderr, "%s: load unexpectedly succeeded\n", what);
    assert(status != LUA_OK);
  }
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  DumpBuf base = { NULL, 0, 0 };
  DumpBuf mod = { NULL, 0, 0 };
  DumpBuf redump = { NULL, 0, 0 };
  uint32_t framesize, numbc;
  size_t bcpos;

  assert(L != NULL);
  luaL_openlibs(L);

  compile_to_dump(L, "return 42", &base);
  assert((uint8_t)base.p[3] == BCDUMP_VERSION_LOCKLESS);

  ljt_lua_assert_ok(L, load_dump(L, &base), "load v4 dump");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "pcall v4 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_TRANS;
  ljt_lua_assert_ok(L, load_dump(L, &mod), "load patched v3 dump");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "pcall patched v3 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_LEGACY;
  ljt_lua_assert_ok(L, load_dump(L, &mod), "load patched v2 dump");
  dump_reset(&redump);
  assert(lua_dump(L, dump_writer, &redump) == 0);
  assert((uint8_t)redump.p[3] == BCDUMP_VERSION_LOCKLESS);
  assert((uint8_t)redump.p[first_proto_offset(&redump)] & BCDUMP_PF_LEGACYUV);
  ljt_lua_assert_ok(L, load_dump(L, &redump), "load redumped v2 dump");
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "pcall redumped v2 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  ljt_lua_assert_ok(L, lua_pcall(L, 0, 1, 0), "pcall patched v2 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_LOCKLESS + 1;
  assert_load_fails(L, &mod, "unknown dump version");

  dump_copy(&mod, &base);
  mod.p[first_proto_offset(&mod)] = PROTO_ILOOP;
  assert_load_fails(L, &mod, "dump with forbidden proto flag");

  bcpos = first_bc_offset(&base, &framesize, &numbc);
  assert(framesize > 0);
  assert(numbc >= 2);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_LEGACY;
  patch_ins(&mod, bcpos, BCINS_AD(BC_CNEW, 0, 0));
  assert_load_fails(L, &mod, "v2 dump with lockless cell opcode");

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_TRANS;
  patch_ins(&mod, bcpos, BCINS_AD(BC_CNEW, 0, 0));
  assert_load_fails(L, &mod, "v3 dump with lockless cell opcode");

  dump_copy(&mod, &base);
  patch_ins(&mod, bcpos, BCINS_AD(BC_CGET, 0, framesize));
  assert_load_fails(L, &mod, "v4 dump with out-of-frame CGET");

  dump_copy(&mod, &base);
  patch_ins(&mod, bcpos, BCINS_AD(BC_CGET, 0, 0));
  assert_load_fails(L, &mod, "v4 dump with self-overwriting CGET");

  dump_copy(&mod, &base);
  patch_ins(&mod, bcpos, BCINS_AD(BC_CNEW, 0, 0));
  patch_ins(&mod, bcpos + sizeof(BCIns), BCINS_AD(BC_UCLO, 1, 0));
  assert_load_fails(L, &mod, "v4 cell dump with closing UCLO");

  compile_to_dump(L, "local x=0; local function g() return x end; "
		   "x=x+1; return x,g", &base);
  ljt_lua_assert_ok(L, load_dump(L, &base), "load v4 CGET/CSET dump");
  assert((top_proto(L)->flags & PROTO_NOJIT) == 0);
  lua_pop(L, 1);

  compile_to_dump(L, "local keep; for i=1,2 do "
		   "local function f() return f end; keep=f end; return keep",
		   &base);
  ljt_lua_assert_ok(L, load_dump(L, &base), "load v4 CNEW dump");
  assert((top_proto(L)->flags & PROTO_NOJIT) == 0);
  lua_pop(L, 1);

  assert_jit_patched_dump_unpatches(L);

  dump_free(&redump);
  dump_free(&mod);
  dump_free(&base);
  lua_close(L);
  printf("t-bcdump-current OK: bytecode dump compatibility guarded\n");
  return 0;
}
