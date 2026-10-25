/* Native release-profile timing for the production rooted scalar read. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach_time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_tab.h"

static mach_timebase_info_data_t timebase;

static uint64_t elapsed_ns(uint64_t start, uint64_t end)
{
  return (uint64_t)(((__uint128_t)(end-start) * timebase.numer) /
                    timebase.denom);
}

static int compare_ns(const void *left, const void *right)
{
  uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static void measure(lua_State *L, const char *name, const char *setup,
                    int want_hit)
{
  enum { WARMUP = 10000, ITERATIONS = 1000000, SAMPLES = 10000 };
  TValue out;
  cTValue *table_root, *key_root;
  uint64_t start, end;
  uint32_t hits = 0, i;
  uint64_t ns, latency[SAMPLES], clock_pair[SAMPLES];
  assert(lua_gettop(L) == 0);
  assert(luaL_dostring(L, setup) == LUA_OK && lua_gettop(L) == 2);
  table_root = L->top - 2;
  key_root = L->top - 1;
  setnumV(&out, -1.5);
  for (i = 0; i < WARMUP; i++)
    assert(lj_tab_getscalar_rooted_try(L, table_root, key_root, &out) == want_hit);
  start = mach_absolute_time();
  for (i = 0; i < ITERATIONS; i++) {
    if (lj_tab_getscalar_rooted_try(L, table_root, key_root, &out)) hits++;
  }
  end = mach_absolute_time();
  ns = elapsed_ns(start, end);
  assert(hits == (want_hit ? ITERATIONS : 0u) && tvisnumber(&out) &&
         numberVnum(&out) == (want_hit ? 2.5 : -1.5));
  /* Sample individual calls separately from throughput. Keep raw wall-time
  ** samples, including timer overhead and scheduling; subtracting quantiles
  ** of a separate clock baseline would invent an adjusted tail latency. */
  for (i = 0; i < SAMPLES; i++) {
    start = mach_absolute_time();
    assert(lj_tab_getscalar_rooted_try(L, table_root, key_root, &out) == want_hit);
    end = mach_absolute_time();
    latency[i] = elapsed_ns(start, end);
    start = mach_absolute_time();
    end = mach_absolute_time();
    clock_pair[i] = elapsed_ns(start, end);
  }
  qsort(latency, SAMPLES, sizeof(latency[0]), compare_ns);
  qsort(clock_pair, SAMPLES, sizeof(clock_pair[0]), compare_ns);
  printf("release rooted scalar read: case=%s iterations=%u hits=%u refusals=%u "
    "elapsed-ns=%llu ns/read=%.2f reads/s=%.0f\n",
    name, ITERATIONS, hits, ITERATIONS-hits, (unsigned long long)ns,
    (double)ns / ITERATIONS,
    (double)ITERATIONS * 1000000000.0 / (double)(ns ? ns : 1));
  printf("individual raw ns: samples=%u p50=%llu p95=%llu p99=%llu max=%llu "
    "clock-pair-p50=%llu clock-pair-p99=%llu\n", SAMPLES,
    (unsigned long long)latency[SAMPLES/2-1],
    (unsigned long long)latency[SAMPLES*95/100-1],
    (unsigned long long)latency[SAMPLES*99/100-1],
    (unsigned long long)latency[SAMPLES-1],
    (unsigned long long)clock_pair[SAMPLES/2-1],
    (unsigned long long)clock_pair[SAMPLES*99/100-1]);
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  assert(mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.denom);
  printf("mach timer ns/tick=%u/%u; raw samples include clock overhead\n",
         timebase.numer, timebase.denom);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  measure(L, "array-hit", "return {2.5}, 1", 1);
  measure(L, "numeric-hash-hit", "return {[0.5]=2.5}, 0.5", 1);
  measure(L, "string-hit", "return {key=2.5}, 'key'", 1);
  measure(L, "boolean-hit", "return {[true]=2.5}, true", 1);
  measure(L, "array-miss", "return {2.5}, 2", 0);
  measure(L, "hash-miss", "return {key=2.5}, 'absent'", 0);
  lua_close(L);
  return 0;
}
