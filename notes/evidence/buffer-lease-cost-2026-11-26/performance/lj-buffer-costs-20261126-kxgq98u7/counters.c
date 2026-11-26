#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_udata.h"

static int enabled;
static uint64_t allocs, allocbytes, frees, freebytes;
static uint64_t leases, releases, active_releases, anchors, pops, marks, rescans;
static struct { size_t size; uint64_t count; } sizes[64];
static unsigned nsizes;

void *__real_lj_mem_new_nothrow(lua_State *, GCSize);
void *__wrap_lj_mem_new_nothrow(lua_State *L, GCSize size)
{
  if (enabled) {
    unsigned i;
    allocs++; allocbytes += size;
    for (i = 0; i < nsizes && sizes[i].size != size; i++) {}
    if (i == nsizes && nsizes < 64) { sizes[i].size = size; nsizes++; }
    if (i < 64) sizes[i].count++;
  }
  return __real_lj_mem_new_nothrow(L, size);
}

void __real_lj_mem_free(global_State *, void *, size_t);
void __wrap_lj_mem_free(global_State *g, void *p, size_t size)
{
  if (enabled) { frees++; freebytes += size; }
  __real_lj_mem_free(g, p, size);
}

int __real_lj_gc2_storage_lease_try(global_State *, const void *, size_t,
                                  LJGC2Lease *, size_t *);
int __wrap_lj_gc2_storage_lease_try(global_State *g, const void *p, size_t minimum,
                                  LJGC2Lease *lease, size_t *span)
{
  if (enabled) leases++;
  return __real_lj_gc2_storage_lease_try(g, p, minimum, lease, span);
}

void __real_lj_gc2_lease_release(LJGC2Lease *);
void __wrap_lj_gc2_lease_release(LJGC2Lease *lease)
{
  if (enabled) { releases++; if (lease->admission) active_releases++; }
  __real_lj_gc2_lease_release(lease);
}

TValue *__real_lj_tg_root_anchor_push(lua_State *, TGState *, cTValue *, uint32_t *);
TValue *__wrap_lj_tg_root_anchor_push(lua_State *L, TGState *tg, cTValue *tv, uint32_t *index)
{
  if (enabled) anchors++;
  return __real_lj_tg_root_anchor_push(L, tg, tv, index);
}

void __real_lj_tg_root_anchor_pop(TGState *, uint32_t);
void __wrap_lj_tg_root_anchor_pop(TGState *tg, uint32_t index)
{
  if (enabled) pops++;
  __real_lj_tg_root_anchor_pop(tg, index);
}

#ifdef CURRENT_BUFFER
int __real_lj_gc2_markmem_registered_publish_try(global_State *, void *);
int __wrap_lj_gc2_markmem_registered_publish_try(global_State *g, void *p)
{
  if (enabled) marks++;
  return __real_lj_gc2_markmem_registered_publish_try(g, p);
}

void __real_lj_udata_rescan(lua_State *, GCudata *);
void __wrap_lj_udata_rescan(lua_State *L, GCudata *ud)
{
  if (enabled) rescans++;
  __real_lj_udata_rescan(L, ud);
}
#endif

int main(int argc, char **argv)
{
  lua_State *L;
  unsigned i;
  if (argc != 6) return 2;
  L = luaL_newstate();
  luaL_openlibs(L);
  lua_newtable(L);
  for (i = 1; i <= 4; i++) {
    lua_pushstring(L, argv[i+1]);
    lua_rawseti(L, -2, i);
  }
  lua_setglobal(L, "arg");
  if (luaL_loadfile(L, argv[1]) || lua_pcall(L, 0, 2, 0)) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 3;
  }
  lua_pushvalue(L, -2);
  enabled = 1;
  if (lua_pcall(L, 0, 0, 0)) {
    enabled = 0;
    fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 4;
  }
  enabled = 0;
  if (lua_pcall(L, 0, 0, 0)) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 5;
  }
  printf("{\"allocs\":%llu,\"allocbytes\":%llu,\"frees\":%llu,\"freebytes\":%llu,"
         "\"storage_leases\":%llu,\"releases\":%llu,\"active_releases\":%llu,"
         "\"anchors\":%llu,\"pops\":%llu,\"marks\":%llu,\"rescans\":%llu,\"sizes\":[",
         (unsigned long long)allocs,(unsigned long long)allocbytes,
         (unsigned long long)frees,(unsigned long long)freebytes,
         (unsigned long long)leases,(unsigned long long)releases,
         (unsigned long long)active_releases,(unsigned long long)anchors,
         (unsigned long long)pops,(unsigned long long)marks,(unsigned long long)rescans);
  for (i = 0; i < nsizes; i++)
    printf("%s[%zu,%llu]",i?",":"",sizes[i].size,(unsigned long long)sizes[i].count);
  puts("]}");
  lua_close(L);
  return 0;
}
