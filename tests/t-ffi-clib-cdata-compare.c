/* Native comparison controls. Only private test stage words are written.
** SMR refusal is supplied by the real pending exclusive-reclaim protocol. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_clib.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"

enum { PROBE_WRONG, PROBE_NULL, PROBE_TABLE, PROBE_KEY,
       PROBE_SMR, PROBE_GC, PROBE_FLUSH, PROBE_KGC };
static int mode;
static global_State *selected_g;
static lua_State *selected_L;
static GCtab *selected_env;
static GCstr *selected_key;
static GCcdata *selected_wrong;
static uint32_t armed, main_waiting, peer_at_close, helper_done, peer_done;
static uintptr_t peer_tid;
static int hits, status_seen, native_seen, scratch_clean, counters_clean;
static int real_native_veto, requests_seen, nonnative_checks;

/* Linux GC2 keeps its SMR nesting cell private. Observe the real public
** admit/release calls instead of inventing access to that TLS state. */
static __thread int tracking, smr_delta, lease_delta, smr_attempts, valid_leases;
static __thread int collecting;
static __thread LJGC2Lease *tokens[2];
extern int __real_lj_gc2_smr_read_try(global_State *g);
int __wrap_lj_gc2_smr_read_try(global_State *g)
{
  int result=__real_lj_gc2_smr_read_try(g);
  if(tracking){smr_attempts++;if(result)smr_delta++;}
  return result;
}
extern void __real_lj_gc2_smr_read_leave(global_State *g);
void __wrap_lj_gc2_smr_read_leave(global_State *g)
{
  if(tracking){assert(smr_delta>0);smr_delta--;}
  __real_lj_gc2_smr_read_leave(g);
}
extern int __real_lj_gc2_tv_lease_acquire(global_State *g,cTValue *tv,LJGC2Lease *lease);
int __wrap_lj_gc2_tv_lease_acquire(global_State *g,cTValue *tv,LJGC2Lease *lease)
{
  int result=__real_lj_gc2_tv_lease_acquire(g,tv,lease);
  if(tracking && result==LJ_GC2_TV_EDGE_VALID){
    int i;for(i=0;i<2 && tokens[i];i++){}
    assert(i<2);tokens[i]=lease;lease_delta++;valid_leases++;
  }
  return result;
}
extern void __real_lj_gc2_lease_release(LJGC2Lease *lease);
void __wrap_lj_gc2_lease_release(LJGC2Lease *lease)
{
  if(tracking){
    int i;for(i=0;i<2 && tokens[i]!=lease;i++){}
    assert(i<2 && lease_delta>0);tokens[i]=NULL;lease_delta--;
  }
  __real_lj_gc2_lease_release(lease);
}
static void counters_begin(void)
{
  assert(!tracking && !smr_delta && !lease_delta && !tokens[0] && !tokens[1]);
  smr_attempts=valid_leases=0;tracking=1;
}
static void counters_end(void)
{
  assert(tracking && !smr_delta && !lease_delta && !tokens[0] && !tokens[1]);
  tracking=0;
}

static void wait_word(uint32_t *word)
{
  while (!la_load32_acq(word)) sched_yield();
}
int reg_cmp_wait_main(void)
{
  wait_word(&main_waiting);
  la_storeuptr_rel(&peer_tid, (uintptr_t)pthread_self());
  return 1;
}
int reg_cmp_peer_done(void) { la_store32_rel(&peer_done, 1); return 1; }

extern int __real_lj_gc2_collect_active(lua_State *L);
int __wrap_lj_gc2_collect_active(lua_State *L)
{
  int result;
  assert(!collecting); collecting = 1;
  result = __real_lj_gc2_collect_active(L);
  collecting = 0;
  return result;
}
extern int __real_lj_tg_any_jit_active(global_State *g);
int __wrap_lj_tg_any_jit_active(global_State *g)
{
  if (mode == PROBE_SMR && g == selected_g &&
      la_loaduptr_acq(&peer_tid) == (uintptr_t)pthread_self() &&
      gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE &&
      gc2_jit_phase_gate_acq(g) == 0) {
    /* The real reclaimer already closed reader admission and must now sample
    ** native activity. Pause before that sample, without changing its gates. */
    la_store32_rel(&peer_at_close, 1);
    wait_word(&helper_done);
    real_native_veto = __real_lj_tg_any_jit_active(g);
    assert(real_native_veto);
    return real_native_veto;
  }
  if (mode == PROBE_GC && collecting && g == selected_g &&
      la_load32_acq(&main_waiting) &&
      la_loaduptr_acq(&peer_tid) == (uintptr_t)pthread_self() &&
      gc2_jit_phase_gate_acq(g) == 0) {
    int actual = __real_lj_tg_any_jit_active(g);
    if (actual) {
      /* Public collection has genuinely closed native admission and sampled
      ** this still-active trace. Hold its real deferral decision while the
      ** main thread performs the actual comparison. No gate is fabricated. */
      real_native_veto = actual;
      la_store32_rel(&peer_at_close, 1);
      wait_word(&helper_done);
    }
    return actual;
  }
  return __real_lj_tg_any_jit_active(g);
}
int reg_cmp_reclaim(void)
{
  uint32_t reclaimed;
  wait_word(&main_waiting);
  la_storeuptr_rel(&peer_tid, (uintptr_t)pthread_self());
  reclaimed = lj_gc2_reclaim_retired(selected_g, lj_gc2_retire_epoch(selected_g));
  assert(reclaimed == 0 && la_load32_acq(&peer_at_close) && real_native_veto);
  assert(gc2_smr_reclaiming_acq(selected_g) == LJ_GC2_SMR_OPEN);
  la_store32_rel(&peer_done, 1);
  return 1;
}

extern int __real_lj_tab_cmpcdata_kgc_rooted_try(
  lua_State *L, cTValue *tabroot, cTValue *keyroot, GCcdata *expected);
int __wrap_lj_tab_cmpcdata_kgc_rooted_try(
  lua_State *L, cTValue *tabroot, cTValue *keyroot, GCcdata *expected)
{
  int match = la_load32_acq(&armed) && L == selected_L && G(L) == selected_g &&
    tvistab(tabroot) && tabV(tabroot) == selected_env &&
    tvisstr(keyroot) && strV(keyroot) == selected_key &&
    lj_tg_load_jit_base(L2TG(L)) != NULL;
  int result;
  if (!match)
    return __real_lj_tab_cmpcdata_kgc_rooted_try(L, tabroot, keyroot, expected);
  {
    LJStateOwner owner = lj_state_owner_word_acq(L);
    uint64_t table_word = tv_rawload_acq(tabroot), key_word = tv_rawload_acq(keyroot);
    GCArena *ta = lj_arena_of(selected_env), *ka = lj_arena_of(selected_key);
    uint64_t table_readers = lj_arena_remote_active_acq(ta);
    uint64_t key_readers = lj_arena_remote_active_acq(ka);
    TValue *native_base = lj_tg_load_jit_base(L2TG(L));
    la_store32_rel(&armed, 0);
    hits++;
    native_seen = native_base != NULL;
    assert(native_seen && expected);
    if (mode == PROBE_WRONG) expected = selected_wrong;
    if (mode == PROBE_NULL) expected = NULL;
#if defined(LJ_GC2_TEST_HELPERS)
    if (mode == PROBE_TABLE || mode == PROBE_KEY)
      lj_gc2_test_stack_admission_retry_once(mode == PROBE_TABLE ?
        obj2gco(selected_env) : obj2gco(selected_key));
#else
    assert(mode != PROBE_TABLE && mode != PROBE_KEY);
#endif
    if (mode == PROBE_SMR || mode == PROBE_GC || mode == PROBE_FLUSH) {
      la_store32_rel(&main_waiting, 1);
      if (mode == PROBE_SMR) {
        wait_word(&peer_at_close);
        assert(gc2_smr_reclaiming_acq(G(L)) == LJ_GC2_SMR_META_EXCLUSIVE);
      } else if (mode == PROBE_GC) {
        wait_word(&peer_at_close);
        requests_seen = gc2_jit_phase_gate_acq(G(L)) == 0;
        assert(requests_seen && real_native_veto && !la_load32_acq(&peer_done));
      } else {
        /* The peer calls public GC/flush. Observe its real asynchronous
        ** request while this TG still owns the native interval. */
        while (gc2_jit_phase_gate_acq(G(L)) != 0 &&
               la_load32_acq(&L2TG(L)->poll) == 0 &&
               !la_load32_acq(&peer_done)) sched_yield();
        requests_seen = gc2_jit_phase_gate_acq(G(L)) == 0 ||
                        la_load32_acq(&L2TG(L)->poll) != 0;
        assert(requests_seen && !la_load32_acq(&peer_done));
      }
    }
    counters_begin();
    result = __real_lj_tab_cmpcdata_kgc_rooted_try(L, tabroot, keyroot, expected);
    counters_end();
    status_seen = result;
    scratch_clean = table_word == tv_rawload_acq(tabroot) && key_word == tv_rawload_acq(keyroot);
    assert(scratch_clean && lj_state_owner_word_acq(L) == owner);
    assert(lj_tg_load_jit_base(L2TG(L)) == native_base);
    counters_clean = 1;
    if(mode==PROBE_NULL)assert(smr_attempts==0 && valid_leases==0);
    else assert(smr_attempts==1);
    if(mode==PROBE_WRONG || mode==PROBE_KGC)assert(valid_leases==2);
    if(mode==PROBE_TABLE || mode==PROBE_SMR)assert(valid_leases==0);
    if(mode==PROBE_KEY)assert(valid_leases==1);
    if (mode != PROBE_GC && mode != PROBE_FLUSH) {
      assert(lj_arena_remote_active_acq(ta) == table_readers);
      assert(lj_arena_remote_active_acq(ka) == key_readers);
    }
#if defined(LJ_GC2_TEST_HELPERS)
    if (mode == PROBE_TABLE || mode == PROBE_KEY)
      assert(lj_gc2_test_stack_admission_retry_hits() == 1);
#endif
    if (mode == PROBE_SMR) {
      assert(result == 0);
      la_store32_rel(&helper_done, 1);
      wait_word(&peer_done);
    } else if (mode == PROBE_GC) {
      la_store32_rel(&helper_done, 1);
    } else if (mode == PROBE_KGC) {
      assert(result == 1);
    } else if (mode != PROBE_GC && mode != PROBE_FLUSH) {
      assert(result == 0);
    }
  }
  return result;
}

static int arm_probe(lua_State *L)
{
  CLibrary *cl;
  LJStateOwner owner = lj_state_owner_word_acq(L);
  uint64_t table_word, key_word;
  GCcdata *expected;
  assert(!la_load32_acq(&armed) && hits == 0);
  assert(tvisudata(L->base) && tvisstr(L->base+1) && tviscdata(L->base+2));
  assert(tvistab(L->base+3) && tviscdata(L->base+4));
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  cl = (CLibrary *)uddata(udataV(L->base));
  selected_g = G(L); selected_L = L;
  selected_env = lj_clib_cache_env_acq(cl); selected_key = strV(L->base+1);
  selected_wrong = cdataV(L->base+2); expected = cdataV(L->base+4);
  assert(tabV(L->base+3) == selected_env && selected_wrong != expected);
  table_word = tv_rawload_acq(L->base+3); key_word = tv_rawload_acq(L->base+1);
  counters_begin();
  assert(!__real_lj_tab_cmpcdata_kgc_rooted_try(L,L->base+3,L->base+1,expected));
  assert(!__real_lj_tab_cmpcdata_kgc_rooted_try(L,L->base+3,L->base+1,NULL));
  assert(!__real_lj_tab_cmpcdata_kgc_rooted_try(L,NULL,L->base+1,expected));
  assert(!__real_lj_tab_cmpcdata_kgc_rooted_try(L,L->base+3,NULL,expected));
  assert(!__real_lj_tab_cmpcdata_kgc_rooted_try(NULL,L->base+3,L->base+1,expected));
  counters_end();assert(smr_attempts==0 && valid_leases==0);
  nonnative_checks = 5;
  assert(table_word == tv_rawload_acq(L->base+3) && key_word == tv_rawload_acq(L->base+1));
  assert(lj_state_owner_word_acq(L) == owner);
  la_store32_rel(&armed, 1);
  return 0;
}
static int witness(lua_State *L)
{
  lua_pushinteger(L,hits); lua_pushinteger(L,status_seen);
  lua_pushinteger(L,native_seen); lua_pushinteger(L,scratch_clean);
  lua_pushinteger(L,counters_clean); lua_pushinteger(L,real_native_veto);
  lua_pushinteger(L,requests_seen); lua_pushinteger(L,nonnative_checks);
  return 8;
}
static int advance_retired(lua_State *L)
{
  global_State *g=G(L);
  (void)lj_gc2_handshake(g,LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_reclaim_retired(g,lj_gc2_retire_epoch(g));
  lua_pushboolean(L,lj_clib_cache_retired_head_acq(g)==NULL);
  return 1;
}
static int closed_cache_empty(lua_State *L)
{
  CLibrary *cl=(CLibrary *)uddata(udataV(L->base));
  lua_pushboolean(L,(lj_clib_lifecycle_acq(cl)&LJ_CLIB_CLOSING)!=0 &&
                    lj_clib_cache_head_acq(cl)==NULL &&
                    lj_clib_cache_retired_head_acq(G(L))==NULL);
  return 1;
}
int main(int argc,char **argv)
{
  const char *names[]={"wrong","null","table-refusal","key-refusal","smr","gc","flush","kgc"};
  lua_State *L; int i,status;
  assert(argc==6 || argc==7); alarm(30);
  for(mode=0;mode<8;mode++) if(strcmp(argv[3],names[mode])==0) break;
  assert(mode<8);
  L=luaL_newstate();assert(L);luaL_openlibs(L);
  lua_pushcfunction(L,arm_probe);lua_setglobal(L,"arm_cmp_probe");
  lua_pushcfunction(L,witness);lua_setglobal(L,"cmp_probe_witness");
  lua_pushcfunction(L,advance_retired);lua_setglobal(L,"advance_retired");
  lua_pushcfunction(L,closed_cache_empty);lua_setglobal(L,"closed_cache_empty");
  lua_newtable(L);
  for(i=1;i<argc;i++){lua_pushstring(L,argv[i]);lua_rawseti(L,-2,i-1);}
  lua_pushliteral(L,"helper");lua_rawseti(L,-2,5);
  if(argc==7){lua_pushstring(L,argv[6]);lua_rawseti(L,-2,6);}
  lua_setglobal(L,"arg");
  status=luaL_loadfile(L,argv[1]);if(!status)status=lua_pcall(L,0,0,0);
  if(status)fprintf(stderr,"%s\n",lua_tostring(L,-1));
  assert(status==0);
  if(mode==PROBE_KGC && argc==7 && strcmp(argv[6],"off")==0)
    assert(hits==0 && nonnative_checks==0);
  else
    assert(hits==1 && native_seen && scratch_clean && counters_clean && nonnative_checks==5);
  assert(!la_load32_acq(&armed));
  if(mode==PROBE_SMR)assert(real_native_veto && la_load32_acq(&peer_done));
  if(mode==PROBE_GC || mode==PROBE_FLUSH)assert(requests_seen && la_load32_acq(&peer_done));
  if(mode==PROBE_GC)assert(real_native_veto);
  lua_close(L);return 0;
}
