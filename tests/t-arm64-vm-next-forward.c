/*
** Focused ARM64 guard for lj_vm_next over current and forwarded table slots.
*/

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/test_sleep.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_vm.h"

#include "lib/tab_forward_helpers.h"

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT
#error "t-arm64-vm-next-forward requires a JIT-enabled macOS ARM64 build"
#endif

#ifndef LJ_TAB_TEST_HELPERS
#error "t-arm64-vm-next-forward requires LJ_TAB_TEST_HELPERS"
#endif

#ifndef __USER_LABEL_PREFIX__
#define __USER_LABEL_PREFIX__
#endif
#define LJ_XSTR_(s)	#s
#define LJ_XSTR(s)	LJ_XSTR_(s)
#define LJ_ASM_SYM(s)	LJ_XSTR(__USER_LABEL_PREFIX__) #s

typedef struct VMNextResult {
  uint64_t val;
  uint64_t key;
  uint32_t next;
  uint32_t unused;
  uint64_t result_delta;
} VMNextResult;

LJ_STATIC_ASSERT(offsetof(VMNextResult, val) == 0);
LJ_STATIC_ASSERT(offsetof(VMNextResult, key) == 8);
LJ_STATIC_ASSERT(offsetof(VMNextResult, next) == 16);
LJ_STATIC_ASSERT(offsetof(VMNextResult, result_delta) == 24);

#if LJ_ABI_BRANCH_TRACK
#define VMNEXT_BTI_ASM	"bti c\n"
#else
#define VMNEXT_BTI_ASM	""
#endif

#if LJ_ABI_PAUTH
#define VMNEXT_PAC_ASM	"pacibsp\n"
#define VMNEXT_RET_ASM	"retab\n"
#else
#define VMNEXT_PAC_ASM	""
#define VMNEXT_RET_ASM	"ret\n"
#endif

/*
** Establish the fixed registers used by a generated ARM64 trace and reserve
** exactly the two TValue result slots promised by IRCALL_lj_vm_next.
*/
__asm__(
  ".text\n"
  ".p2align 2\n"
  ".globl " LJ_ASM_SYM(lj_test_arm64_vm_next_call) "\n"
  LJ_ASM_SYM(lj_test_arm64_vm_next_call) ":\n"
  VMNEXT_BTI_ASM
  VMNEXT_PAC_ASM
  "sub sp, sp, #64\n"
  "stp x19, x20, [sp, #16]\n"
  "stp x22, x25, [sp, #32]\n"
  "stp x29, x30, [sp, #48]\n"
  "add x29, sp, #48\n"
  "mov x19, x4\n"
  "mov x22, x3\n"
  "mov x25, x2\n"
  "bl " LJ_ASM_SYM(lj_vm_next) "\n"
  "ldp x8, x9, [x0]\n"
  "str x8, [x19]\n"
  "str x9, [x19, #8]\n"
  "str w1, [x19, #16]\n"
  "mov x9, sp\n"
  "sub x8, x0, x9\n"
  "str x8, [x19, #24]\n"
  "ldp x29, x30, [sp, #48]\n"
  "ldp x22, x25, [sp, #32]\n"
  "ldp x19, x20, [sp, #16]\n"
  "add sp, sp, #64\n"
  VMNEXT_RET_ASM
);

LJ_ASMF void lj_test_arm64_vm_next_call(GCtab *t, uint32_t idx,
					 void *dispatch, global_State *g,
					 VMNextResult *result);

typedef struct VMNextArrayReleaseCtx {
  GCtab *t;
  TValue *array;
  MSize asize;
  pthread_t thread;
} VMNextArrayReleaseCtx;

typedef struct VMNextNodeReleaseCtx {
  GCtab *t;
  Node *node;
  MSize hmask;
  pthread_t thread;
} VMNextNodeReleaseCtx;

static void vmnext_wait_for_retry(void)
{
  uint32_t attempt;
  for (attempt = 0; attempt < 100000u; attempt++) {
    if (lj_tab_test_wait_no_l_calls() != 0)
      return;
    sleep_ns(100000L);
  }
  assert(lj_tab_test_wait_no_l_calls() != 0);
}

static void *vmnext_publish_array_after_retry(void *arg)
{
  VMNextArrayReleaseCtx *ctx = (VMNextArrayReleaseCtx *)arg;
  vmnext_wait_for_retry();
  lj_tab_array_rel(ctx->t, ctx->array);
  lj_tab_asize_rel(ctx->t, ctx->asize);
  return NULL;
}

static void *vmnext_publish_node_after_retry(void *arg)
{
  VMNextNodeReleaseCtx *ctx = (VMNextNodeReleaseCtx *)arg;
  vmnext_wait_for_retry();
  lj_tab_node_rel(ctx->t, ctx->node);
  lj_tab_hmask_rel(ctx->t, ctx->hmask);
  return NULL;
}

static uint32_t call_vm_next(lua_State *L, GCtab *t, uint32_t idx,
			     TValue *val, TValue *key)
{
  TGState *tg = L2TG(L);
  VMNextResult result;
  assert(tg != NULL);
  memset(&result, 0, sizeof(result));
  lj_test_arm64_vm_next_call(t, idx, (void *)tg->dispatch, G(L), &result);
  assert(result.result_delta == 0);
  val->u64 = result.val;
  key->u64 = result.key;
  return result.next;
}

static void assert_int(cTValue *tv, int32_t want)
{
  assert(tvisnumber(tv));
  assert((tvisint(tv) ? intV(tv) : (int32_t)numV(tv)) == want);
}

static void assert_visible_pair(cTValue *val, cTValue *key)
{
  assert(!tvistabinternal(val));
  assert(!tvistabinternal(key));
}

static void exercise_colocated_and_end(lua_State *L)
{
  GCtab *t;
  TValue val, key;
  uint32_t next;

  lua_createtable(L, 4, 0);
  t = tabV(L->top-1);
  assert(!lj_tab_array_separated(t));
  tabfwd_set_int(L, t, 1, 6001);
  next = call_vm_next(L, t, 0, &val, &key);
  assert(next == 2);
  assert_int(&val, 6001);
  assert_int(&key, 1);
  assert_visible_pair(&val, &key);

  mt_entering_add_acqrel(G(L), 1);
  next = call_vm_next(L, t, 0, &val, &key);
  mt_entering_sub_acqrel(G(L), 1);
  assert(next == 2);
  assert_int(&val, 6001);
  assert_int(&key, 1);
  assert_visible_pair(&val, &key);
  lua_pop(L, 1);

  lua_createtable(L, 0, 0);
  t = tabV(L->top-1);
  next = call_vm_next(L, t, 0, &val, &key);
  assert(next == 1);
  assert(tvisnil(&key));
  lua_pop(L, 1);
}

static void exercise_array_forward(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue val, key;
  MSize oldasize, newasize, oldacap;
  int32_t target = 3;
  MSize i;
  uint32_t next;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)target < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 7100;
    tabfwd_set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  assert(tabfwd_get_i32(t, target) == target + 7100);

  next = call_vm_next(L, t, (uint32_t)target, &val, &key);
  assert(next == (uint32_t)target + 1u);
  assert_int(&val, target + 7100);
  assert_int(&key, target);
  assert_visible_pair(&val, &key);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  next = call_vm_next(L, t, (uint32_t)target, &val, &key);
  assert(next != (uint32_t)-1);
  assert_int(&val, target + 7100);
  assert_visible_pair(&val, &key);

  {
    VMNextArrayReleaseCtx ctx;
    int32_t want = target + 7100;
    lj_tab_storeint(L, &oldarray[target], target + 9000);
    lj_tab_storeint(L, &newarray[target], want);
    lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
    lj_tab_asize_rel(t, oldasize);
    lj_tab_array_rel(t, oldarray);
    ctx.t = t;
    ctx.array = newarray;
    ctx.asize = newasize;
    lj_tab_test_reset_wait_no_l_calls();
    assert(pthread_create(&ctx.thread, NULL,
			  vmnext_publish_array_after_retry, &ctx) == 0);
    next = call_vm_next(L, t, (uint32_t)target, &val, &key);
    assert(pthread_join(ctx.thread, NULL) == 0);
    assert(lj_tab_test_wait_no_l_calls() != 0);
    assert(next != (uint32_t)-1);
    assert_int(&val, want);
    assert_visible_pair(&val, &key);
  }

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_asize_rel(t, 0);
  next = call_vm_next(L, t, (uint32_t)target, &val, &key);
  assert(next != (uint32_t)-1);
  assert_int(&val, target + 7100);
  assert_visible_pair(&val, &key);
  lj_tab_asize_rel(t, newasize);
  lua_pop(L, 1);
}

static void exercise_hash_forward(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  Node *oldnode, *newnode, *oldn;
  TValue val, key, keylock, savedkey;
  MSize oldhmask, newhmask;
  uint32_t idx, next, seq = 0;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  hkey = tabfwd_find_sid_bucket(L, "vm_next_forward_field",
				(oldhmask << 1) | 1u, oldhmask >> 1,
				&seq);
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 8181);
  assert(lj_tab_node_acq(t) == oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);
  lj_tv_load_acq(&savedkey, &oldn->key);
  idx = lj_tab_asize_acq(t) + (uint32_t)(oldn - oldnode);

  next = call_vm_next(L, t, idx, &val, &key);
  assert(next == idx + 1u);
  assert_int(&val, 8181);
  assert(tvisstr(&key) && strV(&key) == hkey);
  assert_visible_pair(&val, &key);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(newhmask == ((oldhmask << 1) | 1u));
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);

  tv_rawstore_rel(&oldn->key, tv_rawload(&savedkey));
  tabfwd_store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  next = call_vm_next(L, t, idx, &val, &key);
  assert(next != (uint32_t)-1);
  assert_int(&val, 8181);
  assert(tvisstr(&key) && strV(&key) == hkey);
  assert_visible_pair(&val, &key);

  setkeylockV(&keylock);
  lj_tab_storeint(L, &oldn->val, 8181);
  tv_rawstore_rel(&oldn->key, tv_rawload(&keylock));
  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);
  next = call_vm_next(L, t, idx, &val, &key);
  assert(next == 0);
  assert(tvisnil(&val));
  assert(tvisnil(&key));
  assert_visible_pair(&val, &key);

  tv_rawstore_rel(&oldn->key, tv_rawload(&savedkey));
  tabfwd_store_forward(&oldn->val);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);
  {
    VMNextNodeReleaseCtx ctx;
    ctx.t = t;
    ctx.node = newnode;
    ctx.hmask = newhmask;
    lj_tab_test_reset_wait_no_l_calls();
    assert(pthread_create(&ctx.thread, NULL,
			  vmnext_publish_node_after_retry, &ctx) == 0);
    next = call_vm_next(L, t, idx, &val, &key);
    assert(pthread_join(ctx.thread, NULL) == 0);
    assert(lj_tab_test_wait_no_l_calls() != 0);
    assert(next != (uint32_t)-1);
    assert_int(&val, 8181);
    assert(tvisstr(&key) && strV(&key) == hkey);
    assert_visible_pair(&val, &key);
  }

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_colocated_and_end(L);
  exercise_array_forward(L);
  exercise_hash_forward(L);

  lua_close(L);
  printf("t-arm64-vm-next-forward OK: fast and forwarded snapshots resolved\n");
  return 0;
}
