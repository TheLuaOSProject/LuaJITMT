#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tab.h"
static int sampled;
static void sample_keys(lua_State *L)
{
  global_State *g = G(L);
  LJGC2Lease parent;
  GCtab *t;
  Node *node;
  unsigned j, max, printed = 0;
  if (sampled || !lua_istable(L, 2) || !gc2_recovery_items_acq(g)) return;
  if (!lj_gc2_smr_read_try(g)) return;
  if (lj_gc2_tv_lease_acquire(g, L->base+1, &parent) != LJ_GC2_TV_EDGE_VALID) {
    lj_gc2_smr_read_leave(g); return;
  }
  t = tabV(L->base+1);
  node = lj_tab_node_acq(t);
  max = lj_tab_hmask_acq(t)+1;
  if (max > 256) max = 256;
  for (j=0; j<max && printed<8; j++) {
    TValue key, value;
    LJGC2Lease held;
    GCArena *a;
    uint32_t cell, recovery;
    lj_tv_load_acq(&value, &node[j].val);
    lj_tv_load_acq(&key, &node[j].key);
    if (tvisnil(&value) || !tvisstr(&key)) continue;
    if (lj_gc2_tv_lease_acquire(g, &key, &held) != LJ_GC2_TV_EDGE_VALID) continue;
    a = (GCArena *)held.arena;
    if (a) {
      /* The expected STR lease validates an exact, non-interior allocation.
      ** Its counted arena admission covers these metadata and header reads. */
      cell = lj_arena_cellof(gcV(&key));
      recovery = lj_arena_recovery_state_acq(a, cell);
      if (recovery || printed == 0) {
        fprintf(stderr,"key_sample,node=%u,gct=%u,len=%u,recovery=%u,lifetime=%u,ready=%u,late=%u,mark=%u,root=%u\n",
                j,(unsigned)gcV(&key)->gch.gct,(unsigned)strV(&key)->len,
                recovery,lj_arena_lifetime_state_acq(a,cell),
                lj_arena_ready_get(a,cell),lj_arena_late_get(a,cell),
                lj_arena_bm_get(a->mark,cell),lj_arena_root_state_acq(a,cell));
        printed++;
      }
    }
    lj_gc2_lease_release(&held);
  }
  lj_gc2_lease_release(&parent);
  lj_gc2_smr_read_leave(g);
  sampled=1;
  fprintf(stderr,"key_sample_done,scanned_nodes=%u,printed=%u\n",j,printed);
}
static int snapshot(lua_State *L)
{
  global_State *g = G(L);
  unsigned asize = 0, hmask = 0;
  int i = (int)lua_tointeger(L, 1);
  if (lua_istable(L, 2)) {
    GCtab *t = tabV(L->base + 1);
    asize = lj_tab_asize_acq(t);
    hmask = lj_tab_hmask_acq(t);
  }
  fprintf(stderr, "%d,cpu=%.6f,phase=%u,cycle=%u,grey=%" PRIu64 ",ssb_pub=%" PRIu64 ",ssb_drain=%" PRIu64 ",assist=%" PRIu64 ",assist_grey=%" PRIu64 ",assist_ssb=%" PRIu64 ",marks=%" PRIu64 ",recovery=%" PRIu64 ",asize=%u,hmask=%u\n",
          i, (double)clock()/CLOCKS_PER_SEC, gc2_phase_acq(g), gc2_cycle_acq(g),
          gc2_grey_top_acq(g)-gc2_grey_bottom_acq(g),
          gc2_ssb_items_published_acq(g),gc2_ssb_items_drained_acq(g),
          gc2_assist_runs_acq(g),gc2_assist_grey_drained_acq(g),
          gc2_assist_ssb_converted_acq(g),gc2_marks_this_round_acq(g),
          gc2_recovery_items_acq(g),asize,hmask);
  fprintf(stderr,"recovery_detail,total=%" PRIu64 ",huge=%" PRIu64 ",main_state=%u,published=%" PRIu64 ",redirtied=%" PRIu64 ",drained=%" PRIu64 ",failed=%u,pending_tables=%u,worker_grey=%" PRIu64 ",worker_ssb=%" PRIu64 "\n",
          gc2_recovery_items_acq(g),gc2_recovery_huge_items_acq(g),
          gc2_recovery_main_state_acq(g),gc2_recovery_published_acq(g),
          gc2_recovery_redirtied_acq(g),gc2_recovery_drained_acq(g),
          gc2_recovery_failed_acq(g),gc2_table_rescan_pending_acq(g),
          gc2_worker_grey_drained_acq(g),gc2_worker_ssb_converted_acq(g));
  sample_keys(L);
  return 0;
}
int luaopen_gcprobe(lua_State *L) { lua_pushcfunction(L,snapshot); return 1; }

#include "lualib.h"
#include "luajit.h"
int main(void)
{
  lua_State *L = luaL_newstate();
  int result;
  if (!L) return 2;
  luaL_openlibs(L);
  luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF);
  luaopen_gcprobe(L);
  lua_setglobal(L,"gcprobe");
  result = luaL_dostring(L, "local probe = gcprobe\nlocal n = 5000\ncollectgarbage(\"collect\")\nprobe(-1)\nlocal t = {}\nfor i = 1, n do\n  t[\"newk\" .. i] = i\n  if i % 250 == 0 then probe(i, t) end\nend\nprobe(n + 1, t)\ncollectgarbage(\"collect\")\nprobe(n + 2, t)\nassert(t[\"newk\" .. n] == n)\n");
  if (result) fprintf(stderr,"Lua error: %s\n",lua_tostring(L,-1));
  lua_close(L);
  return result;
}
