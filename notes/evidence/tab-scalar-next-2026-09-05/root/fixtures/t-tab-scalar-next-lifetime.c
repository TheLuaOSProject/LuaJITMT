/* Additional ownership/retirement checks; common immutable v6 oracles. */
#define main authority_fixture_main
#include "t-tab-scalar-next-authority-v6.c"
#undef main

typedef struct ForeignCtx { lua_State *L; int status; TValue k, v; uint32_t idx; } ForeignCtx;
static void *foreign_attempt(void *arg)
{
  ForeignCtx *c = (ForeignCtx *)arg;
  c->status = lj_tab_test_nextscalar_rooted_try(c->L, c->L->base, c->L->base+1,
                                              &c->k, &c->v, &c->idx);
  return NULL;
}
static void preflight(void)
{
  lua_State *L = new_state(0); uint32_t idx = 719;
  TValue *t = L->base, *k = L->base+1, *ok = L->base+2, *ov = L->base+3;
  uint64_t dirty = lj_tg_stack_dirty_epoch_acq(L2TG(L));
  uint32_t readers = gc2_smr_readers_acq(G(L)), anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t waits = lj_tab_test_wait_l_calls(), no_l = lj_tab_test_wait_no_l_calls();
  LJStateOwner owner = lj_state_owner_word_acq(L);
  pthread_t worker; ForeignCtx c;
  assert(lj_tab_test_nextscalar_rooted_try(NULL,t,k,ok,ov,&idx) == -2);
  assert(lj_tab_test_nextscalar_rooted_try(L,NULL,k,ok,ov,&idx) == -2);
  assert(lj_tab_test_nextscalar_rooted_try(L,t,NULL,ok,ov,&idx) == -2);
  assert(lj_tab_test_nextscalar_rooted_try(L,t,k,NULL,ov,&idx) == -2);
  assert(lj_tab_test_nextscalar_rooted_try(L,t,k,ok,NULL,&idx) == -2);
  assert(idx == 719 && numberVnum(ok) == 998 && numberVnum(ov) == 999);
  /* Negative metadata injection; zero never authorizes a successful read. */
  lj_state_owner_word_rel(L, 0); assert(probe(L, &idx) == -2);
  lj_state_owner_word_rel(L, owner);
  c.L=L; c.status=99; setintV(&c.k,998); setintV(&c.v,999); c.idx=719;
  assert(pthread_create(&worker,NULL,foreign_attempt,&c) == 0);
  assert(pthread_join(worker,NULL) == 0);
  assert(c.status == -2 && c.idx == 719 && numberVnum(&c.k) == 998 && numberVnum(&c.v) == 999);
  assert(lj_state_owner_word_acq(L) == owner);
  assert(lj_tg_stack_dirty_epoch_acq(L2TG(L)) == dirty);
  assert(gc2_smr_readers_acq(G(L)) == readers && lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  assert(lj_tab_test_wait_l_calls() == waits && lj_tab_test_wait_no_l_calls() == no_l);
  setforwardV(t); assert(probe(L, &idx) == -2); *t = *(L->base+4);
  assert(probe(L, NULL) == 1 && numberVnum(ov) == 7);
  lua_close(L);
}

static void retired_vector(void)
{
  lua_State *L = new_state(1); GCtab *t = tabV(L->base);
  TValue *old = lj_tab_array_acq(t), *current;
  GCArena *oldarena = lj_arena_of(lj_tab_array_hdr(old));
  uint32_t oldcell = lj_arena_cellof(lj_tab_array_hdr(old)), i, idx = 719;
  assert(!lj_tab_array_is_colocated(t, old));
  lj_tab_resize(L, t, 200, 0); current = lj_tab_array_acq(t);
  assert(current != old && lj_tab_array_is_retiring(t, old));
  /* Real retired generation, still owned by the actual retire queue. */
  lj_tab_array_rel(t, old); assert(probe(L, &idx) == -2); lj_tab_array_rel(t, current);
  for (i=0; i<LJ_TAB_RETIRE_EPOCHS+3u; i++) (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE && lj_tab_array_retired_head_acq(G(L)) == NULL);
  assert(lj_arena_of(lj_tab_array_hdr(current)) == oldarena);
  /* The current live vector retains this mapping, while the old allocation
  ** start is now physically free. The helper may only consult metadata. */
  assert(!lj_arena_bm_get(oldarena->block, oldcell));
  lj_tab_array_rel(t, old); assert(probe(L, &idx) == -2); lj_tab_array_rel(t, current);
  assert(probe(L, &idx) == 1 && numberVnum(L->base+3) == 7);
  lua_close(L);
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  if (!strcmp(argv[1],"preflight")) preflight();
  else { assert(!strcmp(argv[1],"retired")); retired_vector(); }
  printf("scalar next lifetime passed: %s\n", argv[1]); return 0;
}
