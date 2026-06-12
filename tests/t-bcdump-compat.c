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

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_bcdump.h"

typedef struct DumpBuf {
  char *p;
  size_t n;
  size_t cap;
} DumpBuf;

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

static void assert_lua_ok(lua_State *L, int status, const char *what)
{
  if (status != LUA_OK) {
    fprintf(stderr, "%s: %s\n", what, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
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
  assert((flags & BCDUMP_F_STRIP) != 0);
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
  assert((flags & BCDUMP_F_STRIP) != 0);
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
  assert_lua_ok(L, luaL_loadstring(L, src), "luaL_loadstring");
  assert(lj_bcwrite(L, top_proto(L), dump_writer, b, BCDUMP_F_STRIP) == 0);
  assert(b->n > 5);
  assert((uint8_t)b->p[3] == BCDUMP_VERSION_LOCKLESS);
  lua_pop(L, 1);
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

  compile_to_dump(L, "return 42", &base);
  assert((uint8_t)base.p[3] == BCDUMP_VERSION_LOCKLESS);

  assert_lua_ok(L, load_dump(L, &base), "load v4 dump");
  assert_lua_ok(L, lua_pcall(L, 0, 1, 0), "pcall v4 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_TRANS;
  assert_lua_ok(L, load_dump(L, &mod), "load patched v3 dump");
  assert_lua_ok(L, lua_pcall(L, 0, 1, 0), "pcall patched v3 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_LEGACY;
  assert_lua_ok(L, load_dump(L, &mod), "load patched v2 dump");
  dump_reset(&redump);
  assert(lua_dump(L, dump_writer, &redump) != 0);
  assert_lua_ok(L, lua_pcall(L, 0, 1, 0), "pcall patched v2 dump");
  assert(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);

  dump_copy(&mod, &base);
  mod.p[3] = BCDUMP_VERSION_LOCKLESS + 1;
  assert_load_fails(L, &mod, "unknown dump version");

  dump_copy(&mod, &base);
  mod.p[first_proto_offset(&mod)] = PROTO_NOJIT;
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
  assert_lua_ok(L, load_dump(L, &base), "load v4 CGET/CSET dump");
  assert((top_proto(L)->flags & PROTO_NOJIT) == 0);
  lua_pop(L, 1);

  compile_to_dump(L, "local keep; for i=1,2 do "
		   "local function f() return f end; keep=f end; return keep",
		   &base);
  assert_lua_ok(L, load_dump(L, &base), "load v4 CNEW dump");
  assert((top_proto(L)->flags & PROTO_NOJIT) != 0);
  lua_pop(L, 1);

  dump_free(&redump);
  dump_free(&mod);
  dump_free(&base);
  lua_close(L);
  printf("t-bcdump-compat OK: v2/v3/v4 bytecode compatibility guarded\n");
  return 0;
}
