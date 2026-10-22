/* Execute the production ARM64 table barrier without opening trace admission. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"

#if defined(__APPLE__) && defined(__aarch64__) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_ARM64_EMIT_TEST_HELPERS)
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_tg.h"

extern void lj_test_arm64_tbar_call(MCode *code, global_State *g,
	ASMFunction *dispatch);

static void unmark(GCobj *o)
{
  GCArena *a = lj_arena_of(o);
  assert(!lj_arena_ishuge(a));
  lj_arena_bm_clear(a->mark, lj_arena_cellof(o));
  assert(!lj_arena_bm_get(a->mark, lj_arena_cellof(o)));
}

static void run_case(lua_State *L, MCode *area, MSize cap, GCtab *tab,
	cTValue *key, int dynamic, int expected_mark, FILE *out)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  MCode *code;
  TValue scratch1, scratch2;
  uint8_t oldmark = tab->marked;
  GCRef grayagain = g->gc.grayagain;
  GCobj *target = key && tvisgcv(key) ? gcV(key) : obj2gco(tab);
  setintV(&scratch1, 0x12345678);
  setintV(&scratch2, -321);
  copyTV(L, &tg->tmptv, &scratch1);
  copyTV(L, &tg->tmptv2, &scratch2);
  unmark(target);
  tab->marked |= LJ_GC_BLACK;
  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(0);
  code = lj_asm_arm64_tbar_test(L2J(L), area, cap, tab, key, dynamic);
  assert(code != NULL && code >= area && code < area+cap);
  lj_mcode_sync(code, area+cap);
  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(1);
  assert(fwrite(code, sizeof(*code), (size_t)(area+cap-code), out) ==
         (size_t)(area+cap-code));
  lj_test_arm64_tbar_call(code, g, tg->dispatch);
  assert((lj_gc2_ismarked(g, target) > 0) == expected_mark);
  assert(tg->tmptv.u64 == scratch1.u64);
  assert(tg->tmptv2.u64 == (key ? key->u64 : scratch2.u64));
  assert(g->gc.grayagain.gcptr64 == grayagain.gcptr64);
  assert((tab->marked & LJ_GC_BLACK) != 0);
  tab->marked = (uint8_t)((tab->marked & ~LJ_GC_BLACK) |
                         (oldmark & LJ_GC_BLACK));
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *strong, *weak;
  TValue key, integer, number;
  MCode *area;
  size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
  FILE *out;
  int dynamic;
  assert(L != NULL && argc == 2 && pagesz >= 4096u);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = L2TG(L);
  lua_newtable(L);
  strong = tabV(L->top-1);
  lua_newtable(L);
  weak = tabV(L->top-1);
  lua_newtable(L);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_newtable(L);
  copyTV(L, &key, L->top-1);
  setintV(&integer, -173);
  setnumV(&number, -173.25);
  area = mmap(NULL, pagesz, PROT_READ|PROT_WRITE|PROT_EXEC,
              MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
  assert(area != MAP_FAILED);
  out = fopen(argv[1], "wb");
  assert(out != NULL);

  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(lj_tg_mark_active_acq(tg));
  run_case(L, area, (MSize)(pagesz/sizeof(MCode)), strong,
           NULL, 0, 1, out);
  for (dynamic = 0; dynamic < 2; dynamic++) {
    run_case(L, area, (MSize)(pagesz/sizeof(MCode)), strong,
             &key, dynamic, 1, out);
    run_case(L, area, (MSize)(pagesz/sizeof(MCode)), weak,
             &key, dynamic, 0, out);
    run_case(L, area, (MSize)(pagesz/sizeof(MCode)), strong,
             &integer, dynamic, 0, out);
  }
  run_case(L, area, (MSize)(pagesz/sizeof(MCode)), strong,
           &number, 1, 0, out);
  /* Restore marks/roots before completing the deliberately perturbed cycle. */
  (void)lj_gc2_markobj(g, obj2gco(strong));
  (void)lj_gc2_markobj(g, gcV(&key));
  setnilV(&tg->tmptv);
  setnilV(&tg->tmptv2);
  lj_gc2_cycle_to_idle(g);
  assert(fclose(out) == 0);
  assert(munmap(area, pagesz) == 0);
  lua_close(L);
  puts("t-arm64-jit-tbar OK: emitted no-key/key GC2 barriers, weak keys, "
       "TG scratch roots and argument-register reuse");
  return 0;
}
#else
int main(void)
{
  puts("t-arm64-jit-tbar SKIP");
  return 0;
}
#endif
