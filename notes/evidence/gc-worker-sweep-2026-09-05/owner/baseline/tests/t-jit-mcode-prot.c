/*
** Focused regression test for mcode execute-stable protection.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#if LJ_TARGET_POSIX
#include <sys/mman.h>
#endif

#if LJ_TARGET_OSX && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0 && \
    defined(MAP_JIT) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 110000
#include <pthread.h>
#define HAS_DARWIN_MAPJIT_MCODE 1
#else
#define HAS_DARWIN_MAPJIT_MCODE 0
#endif

#if defined(__linux__) && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0
typedef struct MapEntry {
  uintptr_t start;
  uintptr_t end;
  char perms[5];
} MapEntry;

static int read_map_for_addr(uintptr_t addr, MapEntry *entry)
{
  FILE *fp = fopen("/proc/self/maps", "r");
  char line[512];
  assert(fp != NULL);
  while (fgets(line, sizeof(line), fp) != NULL) {
    unsigned long long start, end;
    char perms[5];
    if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 &&
        addr >= (uintptr_t)start && addr < (uintptr_t)end) {
      entry->start = (uintptr_t)start;
      entry->end = (uintptr_t)end;
      memcpy(entry->perms, perms, sizeof(entry->perms));
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);
  return 0;
}

static int overlaps(uintptr_t a0, uintptr_t a1, uintptr_t b0, uintptr_t b1)
{
  return a0 < b1 && b0 < a1;
}

static void assert_no_wx_mcode_map(uintptr_t rx, uintptr_t rw, size_t size)
{
  FILE *fp = fopen("/proc/self/maps", "r");
  char line[512];
  uintptr_t rxend = rx + size;
  uintptr_t rwend = rw + size;
  assert(fp != NULL);
  while (fgets(line, sizeof(line), fp) != NULL) {
    unsigned long long start, end;
    char perms[5];
    if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 &&
        (overlaps((uintptr_t)start, (uintptr_t)end, rx, rxend) ||
         overlaps((uintptr_t)start, (uintptr_t)end, rw, rwend))) {
      assert(!(perms[1] == 'w' && perms[2] == 'x'));
    }
  }
  fclose(fp);
}

static void assert_split_mcode_maps(MCode *rx)
{
  MCode *rw;
  size_t size;
  MapEntry rxmap, rwmap;

  assert(rx != NULL);
  size = ((MCLink *)rx)->size;
  rw = lj_mcode_area_rw(rx);
  assert(size != 0);
  assert(rw != NULL);
  assert(rw != rx);

  assert(read_map_for_addr((uintptr_t)rx, &rxmap));
  assert(read_map_for_addr((uintptr_t)rw, &rwmap));
  assert(rxmap.perms[1] != 'w');
  assert(rxmap.perms[2] == 'x');
  assert(rwmap.perms[1] == 'w');
  assert(rwmap.perms[2] != 'x');
  assert_no_wx_mcode_map((uintptr_t)rx, (uintptr_t)rw, size);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do assert(f(120) == 7260) end\n");

#if defined(__linux__) && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0
  {
    jit_State *J = G2J(G(L));
    MCode *rx = J->mcarea;
    MCode *area;

    assert_split_mcode_maps(rx);
    area = lj_mcode_patch(J, rx, 0);
    assert(area == rx);
    assert_split_mcode_maps(rx);
    assert(lj_mcode_patch(J, area, 1) == NULL);
    assert_split_mcode_maps(rx);
  }
#elif HAS_DARWIN_MAPJIT_MCODE
  {
    jit_State *J = G2J(G(L));
    MCode *rx = J->mcarea;
    assert(rx != NULL);
    assert(lj_mcode_area_rw(rx) == rx);
    assert(((MCLink *)rx)->size != 0);
  }
#endif

  lua_close(L);
#if defined(__linux__) && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0
  printf("t-jit-mcode-prot OK: mcode mappings are W^X separated\n");
#elif HAS_DARWIN_MAPJIT_MCODE
  printf("t-jit-mcode-prot OK: macOS MAP_JIT mcode is execute-stable (pthread_jit_write_protect_supported_np=%d)\n",
         pthread_jit_write_protect_supported_np());
#else
  printf("t-jit-mcode-prot OK: mcode protection check skipped on this target\n");
#endif
  return 0;
}
