/* Read-only main-owner diagnostics; links the unchanged frozen archive. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_str.h"

static uint32_t count_owned_list(GCArena *a)
{
  uint32_t n=0;
  for (; a; a=lj_arena_next_acq(a)) {
    n++;
    assert(n < LJ_GC2_ROOT_SCAN_LIMIT);
  }
  return n;
}

static uint32_t count_empty_reclaimed(GCArena *a)
{
  uint32_t n=0;
  for (; a; a=lj_arena_next_acq(a)) {
    uint32_t w;
    uint64_t block=0;
    for (w=0;w<LJ_ARENA_WORDS;w++) block |= la_load64_acq(&a->block[w]);
    if (!block) n++;
  }
  return n;
}

static uint32_t count_certified_reclaimed(GCArena *a)
{
  uint32_t n = 0;
#ifdef LJ_AF_EMPTY_RECLAIMED
  for (; a; a = lj_arena_next_acq(a))
    if (lj_arena_flags_acq(a) & LJ_AF_EMPTY_RECLAIMED) n++;
#else
  (void)a;
#endif
  return n;
}

static int gcdiag(lua_State *L)
{
  global_State *g=G(L);
  TGAlloc *a=&g->main_tg->alloc;
  GC2StatsSnapshot s;
  uint32_t k;
  assert(gc2_n_workers_acq(g)==0 && !mt_active_or_entering_acq(g));
  lj_gc2_stats_snapshot(g,&s);
  printf("gcdiag stage=%s run=%d",lua_tostring(L,1),(int)lua_tointeger(L,2));
#define FIELD(name) printf(" " #name "=%" PRIu64,(uint64_t)s.name)
  FIELD(phase);FIELD(total_bytes);FIELD(cycle_starts);FIELD(major_root_scans);
  FIELD(alloc_total_bytes);FIELD(trigger_bytes);FIELD(hard_bytes);FIELD(live_estimate);
  FIELD(jit_hard_checks);FIELD(interp_hard_checks);FIELD(worker_runs);
  FIELD(worker_grey_drained);FIELD(worker_ssb_converted);FIELD(assist_runs);
  FIELD(sweep_owner_runs);FIELD(sweep_owner_arenas);FIELD(sweep_owner_live_cells);
  FIELD(root_spine_objects);FIELD(root_spine_tombstones);FIELD(pending_root_flushed);
  FIELD(recovery_published);FIELD(recovery_drained);
#undef FIELD
  printf(" strings=%u strmask=%u huge_capacity=%u jit_gate=%u mark_resume=%u",
    lj_str_num_acq(g),lj_str_mask_acq(g),lj_arena_hugetab_slot_count(&g->main_tg->huge),
    gc2_jit_phase_gate_acq(g),gc2_jit_mark_resume_acq(g));
  for (k=0;k<LJ_ARENA_NKINDS;k++)
    printf(" k%u_owned=%u k%u_need=%u k%u_quarantine=%u k%u_reclaimed=%u",k,
      count_owned_list(a->owned[k]),k,count_owned_list(a->needsweep[k]),k,
      count_owned_list(a->quarantine[k]),k,
      count_owned_list(lj_arena_alloc_reclaimed_head(a,k)));
  printf(" empty_reclaimed=%u small_registry_capacity=%u",
    count_empty_reclaimed(lj_arena_alloc_reclaimed_head(a,LJ_ARENAK_TRAVERSABLE)),
    lj_arena_hugetab_slot_count((HugeTab *)gc2_small_arena_tab_acq(g)));
  printf(" certified_reclaimed=%u",
    count_certified_reclaimed(lj_arena_alloc_reclaimed_head(a,LJ_ARENAK_TRAVERSABLE)));
  putchar('\n');fflush(stdout);
  return 0;
}

int main(int argc,char **argv)
{
  lua_State *L;
  int i,rc=0;
  assert(argc>=2);
  setvbuf(stdout,NULL,_IOLBF,0);
  L=luaL_newstate();assert(L);
  luaL_openlibs(L);
  luaJIT_setmode(L,0,LUAJIT_MODE_ENGINE|LUAJIT_MODE_ON);
  lua_pushcfunction(L,gcdiag);lua_setglobal(L,"gcdiag");
  lua_newtable(L);
  for (i=1;i<argc;i++) {lua_pushstring(L,argv[i]);lua_rawseti(L,-2,i-1);}
  lua_setglobal(L,"arg");
  if (luaL_loadfile(L,argv[1]) || lua_pcall(L,0,0,0)) {
    fprintf(stderr,"%s\n",lua_tostring(L,-1));rc=1;
  }
  lua_close(L);
  return rc;
}
