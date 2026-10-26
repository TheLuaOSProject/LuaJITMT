#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_target.h"
#include "lj_bc.h"
#include "lj_ircall.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lib/arm64_trace_fixture.h"

#if !LJ_TARGET_ARM64 || !defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#error "nil-store admission fixture requires experimental ARM64"
#endif
static int semok(Arm64TraceFixture *f)
{ LJArm64IRReject why; return lj_asm_arm64_ir_admit(f->J, &f->semantic, &why); }
static int postok(Arm64TraceFixture *f)
{ IRRef n=0; return lj_asm_arm64_postra_admit(&f->view, &n); }

static unsigned native_deletions;
static void observe_present_delete(lua_State *L, GCtab *t, TValue *dst,
                                   cTValue *key, cTValue *value)
{
  UNUSED(t); UNUSED(dst); UNUSED(key);
  assert(tvisnil(value));
  if (lj_tg_load_jit_base(L2TG(L)) != NULL) native_deletions++;
  lj_tab_test_set_nil_store_found_hook(observe_present_delete);
}

int main(int argc, char **argv)
{
  lua_State *L=luaL_newstate(); Arm64TraceFixture f;
  IRRef r, call=0, carg, table, key, guard; SnapNo after=0; SnapNo s;
  int a,b,forl,matrix;
  assert(L && argc==3);
  forl=!strcmp(argv[2],"forl");
  matrix=!strcmp(argv[2],"matrix");
  assert(forl || matrix || !strcmp(argv[2],"loop"));
  luaL_openlibs(L);
  lua_pushboolean(L,!matrix); lua_setglobal(L,"rooted_nil_store_admission");
  lua_pushboolean(L,forl); lua_setglobal(L,"rooted_nil_forl_admission");
  lua_pushlightuserdata(L, &native_deletions);
  lua_setglobal(L, "rooted_nil_lightud");
  lj_tab_test_set_nil_store_found_hook(observe_present_delete);
  if (luaL_dofile(L,argv[1])) { fprintf(stderr,"%s\n",lua_tostring(L,-1)); abort(); }
  lj_tab_test_set_nil_store_found_hook(NULL);
  assert(native_deletions >= 10);
  printf("rooted nil stores observed %u native present-key deletions\n",
         native_deletions);
  if (matrix) { lua_close(L); return 0; }
  arm64_fixture_init(&f,L,"rooted_nil_store_trace_clone");
  assert(bc_op(f.view.startins) == (forl ? BC_FORL : BC_LOOP));
  assert(semok(&f) && postok(&f));
  for (r=REF_FIRST;r<f.semantic_nins;r++)
    if (f.ir[r].o==IR_CALLS && f.ir[r].op2==IRCALL_lj_tab_storenil_rooted) { call=r; break; }
  assert(call && f.ir[call].op1>REF_FIRST);
  carg=f.ir[call].op1; assert(f.ir[carg].o==IR_CARG);
  table=f.ir[carg].op1; key=f.ir[carg].op2; guard=call+1;
  for (s=0;s<f.semantic.nsnap;s++)
    if (f.snaps[s].ref>call && f.snaps[s].ref<f.semantic_nins) { after=s; break; }
  assert(after);
#define REJECT_BOTH(change) do { \
    arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f)); change; \
    a=semok(&f); b=postok(&f); if(a||b){fprintf(stderr,"both accepted line %d\n",__LINE__);abort();} \
  } while(0)
#define BOTH(ref,field,value) f.ir[(ref)].field=(value); f.postra[(ref)].field=(value)
  REJECT_BOTH(BOTH(call,op2,IRCALL_lj_tab_storetv_existing_forjit));
  REJECT_BOTH(BOTH(guard,o,IR_EQ));
  REJECT_BOTH(BOTH(guard,op1,REF_NIL));
  REJECT_BOTH(BOTH(key,op2,IRTMPREF_IN1));
  REJECT_BOTH(BOTH(carg,op1,key));
  REJECT_BOTH(BOTH(carg,op2,table));
  REJECT_BOTH(BOTH(table,op2,IRTMPREF_IN2));
  REJECT_BOTH(BOTH(table,op2,IRTMPREF_IN3));
  REJECT_BOTH(BOTH(key,op1,REF_NIL));
  REJECT_BOTH(BOTH(f.ir[key].op1,t.irt,IRT_P64|IRT_GUARD));
  REJECT_BOTH(BOTH(f.ir[key].op1,t.irt,IRT_PROTO|IRT_GUARD));
  REJECT_BOTH(BOTH(f.ir[key].op1,t.irt,IRT_STR));
  REJECT_BOTH(BOTH(f.ir[guard].op2,i,17));
  REJECT_BOTH(f.snaps[after].ref=call);
  /* Each independent checker rejects the snapshot corruption on its own. */
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.snaps[after].ref=call; assert(!semok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.snaps[after].ref=call; assert(!postok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.postra[key].r=f.postra[table].r; assert(semok(&f)&&!postok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.postra[call].r=RID_D0; assert(semok(&f)&&!postok(&f));
#undef BOTH
#undef REJECT_BOTH
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  arm64_fixture_free(&f); lua_close(L);
  puts("rooted nil store independent semantic/postRA checks OK"); return 0;
}
