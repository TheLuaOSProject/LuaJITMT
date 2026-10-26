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
#error "rooted insertion admission fixture requires experimental ARM64"
#endif

static int semok(Arm64TraceFixture *f)
{ LJArm64IRReject why; return lj_asm_arm64_ir_admit(f->J, &f->semantic, &why); }
static int postok(Arm64TraceFixture *f)
{ IRRef n=0; return lj_asm_arm64_postra_admit(&f->view, &n); }
static int rooted_insert_lightud_token;
static uint32_t rooted_insert_native_calls;
static TValue expected_table, expected_key, expected_value;
static int observer_armed;

static int arm_native_insert(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  assert(lua_gettop(L) == 3);
  /* Comparison bits only: Lua retains these inputs through every invocation.
  ** The observer never dereferences the copied GC payloads. */
  lj_tv_load_acq(&expected_table, L->base);
  lj_tv_load_acq(&expected_key, L->base+1);
  lj_tv_load_acq(&expected_value, L->base+2);
  observer_armed = 1;
  return 0;
}

static int same_visible_value(cTValue *left, cTValue *right)
{
  return tv_rawload(left) == tv_rawload(right) ||
    (tvisnumber(left) && tvisnumber(right) && numberVnum(left) == numberVnum(right));
}

static void observe_native_insert(lua_State *L, TValue *tabroot,
				  TValue *keyroot, TValue *valueroot)
{
  if (observer_armed && lj_tg_load_jit_base(L2TG(L)) != NULL) {
    assert(same_visible_value(tabroot, &expected_table));
    assert(same_visible_value(keyroot, &expected_key));
    assert(same_visible_value(valueroot, &expected_value));
    rooted_insert_native_calls++;
  }
}

int main(int argc, char **argv)
{
  lua_State *L=luaL_newstate(); Arm64TraceFixture f;
  IRRef r, call=0, outer, inner, table, key, value, valroot;
  IRRef valproducer, keyproducer, guard;
  SnapNo after=0, s;
  int a,b;
  assert(L && argc==5);
  luaL_openlibs(L);
  lua_pushlightuserdata(L,&rooted_insert_lightud_token);
  lua_setglobal(L,"rooted_insert_lightud");
  lua_pushstring(L,argv[2]); lua_setglobal(L,"rooted_insert_admission");
  lua_pushstring(L,argv[3]); lua_setglobal(L,"rooted_insert_opcode");
  lua_pushstring(L,argv[4]); lua_setglobal(L,"rooted_insert_keykind");
  lua_pushcfunction(L, arm_native_insert);
  lua_setglobal(L,"rooted_insert_arm");
  lj_tab_test_set_rooted_gc_store_hook(observe_native_insert);
  if (luaL_dofile(L,argv[1])) { fprintf(stderr,"%s\n",lua_tostring(L,-1)); abort(); }
  lj_tab_test_set_rooted_gc_store_hook(NULL);
  assert(rooted_insert_native_calls > 0);
  arm64_fixture_init(&f,L,"rooted_insert_trace_clone");
  assert(bc_op(f.view.startins)==
	 (strcmp(argv[3],"forl")==0 ? BC_FORL : BC_LOOP));
  assert(semok(&f)&&postok(&f));
  for(r=REF_FIRST;r<f.semantic_nins;r++)
    if(f.ir[r].o==IR_CALLS && f.ir[r].op2==IRCALL_lj_tab_storenew_rooted) {
      call=r; break;
    }
  assert(call && call+1<f.semantic_nins && f.ir[call].op1>REF_FIRST);
  outer=f.ir[call].op1; assert(f.ir[outer].o==IR_CARG);
  inner=f.ir[outer].op1; assert(inner>REF_FIRST && f.ir[inner].o==IR_CARG);
  table=f.ir[inner].op1; key=f.ir[inner].op2; value=outer;
  valroot=f.ir[outer].op2;
  valproducer=f.ir[valroot].op1; keyproducer=f.ir[key].op1;
  assert(table<key && key<outer && valroot>REF_FIRST &&
         valproducer>=f.semantic.nk && keyproducer>=f.semantic.nk);
  guard=call+1;
  for(s=0;s<f.semantic.nsnap;s++)
    if(f.snaps[s].ref>call && f.snaps[s].ref<f.semantic_nins) { after=s; break; }
  assert(after);
#define BOTH(ref,field,val) do { f.ir[(ref)].field=(val); f.postra[(ref)].field=(val); } while(0)
#define REJECT_BOTH(change) do { \
    arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f)); change; \
    a=semok(&f); b=postok(&f); if(a||b){fprintf(stderr,"both accepted line %d\n",__LINE__);abort();} \
  } while(0)
  REJECT_BOTH(BOTH(call,op2,IRCALL_lj_tab_storebool_rooted));
  REJECT_BOTH(BOTH(guard,o,IR_EQ));
  REJECT_BOTH(BOTH(guard,op1,REF_NIL));
  REJECT_BOTH(BOTH(table,op2,IRTMPREF_IN2));
  REJECT_BOTH(BOTH(key,op2,IRTMPREF_IN1));
  REJECT_BOTH(BOTH(outer,op2,inner));
  REJECT_BOTH(BOTH(inner,op1,key));
  REJECT_BOTH(BOTH(inner,op2,table));
  REJECT_BOTH(BOTH(value,op2,table));
  REJECT_BOTH(BOTH(valroot,op2,IRTMPREF_IN1));
  REJECT_BOTH(BOTH(valroot,op2,IRTMPREF_IN2));
  REJECT_BOTH(BOTH(valroot,t.irt,IRT_PROTO|IRT_GUARD));
  if (keyproducer >= REF_FIRST) {
    REJECT_BOTH(BOTH(keyproducer,t.irt,IRT_P64|IRT_GUARD));
    REJECT_BOTH(BOTH(keyproducer,t.irt,IRT_PROTO|IRT_GUARD));
    REJECT_BOTH(BOTH(keyproducer,t.irt,IRT_STR));
  }
  REJECT_BOTH(BOTH(key,op1,REF_NIL));
  if (valproducer >= REF_FIRST)
    REJECT_BOTH(BOTH(valproducer,t.irt,IRT_PROTO|IRT_GUARD));
  REJECT_BOTH(BOTH(valroot,op1,REF_NIL));
  REJECT_BOTH(BOTH(f.ir[guard].op2,i,17));
  REJECT_BOTH(f.snaps[after].ref=call);
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.snaps[after].ref=call; assert(!semok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.snaps[after].ref=call; assert(!postok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.postra[valroot].r=f.postra[table].r; assert(semok(&f)&&!postok(&f));
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  f.postra[call].r=RID_D0; assert(semok(&f)&&!postok(&f));
#undef REJECT_BOTH
#undef BOTH
  arm64_fixture_reset(&f); assert(semok(&f)&&postok(&f));
  arm64_fixture_free(&f); lua_close(L);
  printf("rooted insertion %s/%s/%s: %u exact native inputs; independent semantic/postRA checks OK\n",
         argv[2], argv[3], argv[4], rooted_insert_native_calls);
  return 0;
}
