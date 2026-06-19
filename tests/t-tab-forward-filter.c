/*
** Focused guard for M5 table FORWARD value filtering.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static int count_next(GCtab *t)
{
  TValue key, out[2];
  int count = 0;
  setnilV(&key);
  while (lj_tab_next(t, &key, out) == 1) {
    assert(!tvistabinternal(&out[0]));
    assert(!tvistabinternal(&out[1]));
    key = out[0];
    count++;
  }
  return count;
}

static GCstr *newstr(lua_State *L, const char *s)
{
  return lj_str_new(L, s, strlen(s));
}

static int32_t tv_i32(cTValue *tv)
{
  assert(tv != NULL);
  assert(tvisnumber(tv));
  return tvisint(tv) ? intV(tv) : (int32_t)numV(tv);
}

static void assert_i32(cTValue *tv, int32_t want)
{
  assert(tv_i32(tv) == want);
}

static void assert_forward(cTValue *tv)
{
  TValue val;
  lj_tv_load_acq(&val, tv);
  assert(tvisforward(&val));
}

static TValue *find_str_slot(Node *node, MSize hmask, const GCstr *key)
{
  Node *n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key)
      return &n->val;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

static TValue *find_num_slot(Node *node, MSize hmask, int32_t key)
{
  TValue k;
  Node *n;
  k.n = (lua_Number)key;
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n)
      return &n->val;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

static TValue *find_key_slot(Node *node, MSize hmask, cTValue *key)
{
  MSize i;
  for (i = 0; i <= hmask; i++) {
    Node *n = &node[i];
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return &n->val;
  }
  return NULL;
}

static void exercise_array_forward_hop(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t tail;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));

  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 101);
  lj_tab_storeint(L, lj_tab_setint(L, t, 2), 202);
  lj_tab_storeint(L, lj_tab_setint(L, t, 3), 303);
  lj_tab_storeint(L, lj_tab_setint(L, t, 4), 404);
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 505);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert(oldasize > 8);
  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert_i32(lj_tab_getint(t, 5), 505);

  store_forward(&oldarray[5]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert_i32(lj_tab_getint(t, 5), 505);
  assert(lj_tab_len(t) == 5);
#if LJ_HASJIT
  assert(lj_tab_len_hint(t, 5) == 5);
#endif
  assert(count_next(t) == 5);
  lj_tab_storeint(L, lj_tab_setint(L, t, 5), 909);
  assert_forward(&oldarray[5]);
  assert_i32(&newarray[5], 909);
  assert_i32(lj_tab_getint(t, 5), 909);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);

  tail = (int32_t)newasize - 1;
  lj_tab_storeint(L, lj_tab_setint(L, t, tail), 606);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  lj_tab_resize(L, t, 6, 4);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(newasize == 6);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert_i32(lj_tab_getint(t, tail), 606);

  store_forward(&oldarray[tail]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  assert_i32(lj_tab_getint(t, tail), 606);
  lj_tab_storeint(L, lj_tab_setint(L, t, tail), 808);
  assert_forward(&oldarray[tail]);
  assert_i32(lj_tab_getint(t, tail), 808);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_hash_forward_hop(lua_State *L)
{
  GCtab *t;
  GCstr *hopstr = newstr(L, "tab-forward-hop-string");
  TValue lightkey;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;
  TValue *oldstrslot, *oldnumslot, *oldkeyslot;

  lua_settop(L, 0);
  lua_createtable(L, 4, 8);
  t = tabV(L->top-1);
  setrawlightudV(&lightkey, (void *)(uintptr_t)0x12345);

  lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 101);
  lj_tab_storeint(L, lj_tab_setint(L, t, 33), 202);
  lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 303);

  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldstrslot = find_str_slot(oldnode, oldhmask, hopstr);
  oldnumslot = find_num_slot(oldnode, oldhmask, 33);
  oldkeyslot = find_key_slot(oldnode, oldhmask, &lightkey);
  assert(oldstrslot != NULL);
  assert(oldnumslot != NULL);
  assert(oldkeyslot != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert_i32(lj_tab_getstr(t, hopstr), 101);
  assert_i32(lj_tab_getint(t, 33), 202);
  assert_i32(lj_tab_get(L, t, &lightkey), 303);

  store_forward(oldstrslot);
  store_forward(oldnumslot);
  store_forward(oldkeyslot);

  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  assert_i32(lj_tab_getstr(t, hopstr), 101);
  assert_i32(lj_tab_getint(t, 33), 202);
  assert_i32(lj_tab_get(L, t, &lightkey), 303);
  assert(count_next(t) == 3);
  lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 404);
  lj_tab_storeint(L, lj_tab_setint(L, t, 33), 505);
  lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 606);
  assert_forward(oldstrslot);
  assert_forward(oldnumslot);
  assert_forward(oldkeyslot);
  assert_i32(find_str_slot(newnode, newhmask, hopstr), 404);
  assert_i32(find_num_slot(newnode, newhmask, 33), 505);
  assert_i32(find_key_slot(newnode, newhmask, &lightkey), 606);
  assert_i32(lj_tab_getstr(t, hopstr), 404);
  assert_i32(lj_tab_getint(t, 33), 505);
  assert_i32(lj_tab_get(L, t, &lightkey), 606);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

static void exercise_hash_to_array_forward_hop(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  Node *oldnode, *newnode;
  MSize oldasize, newasize, oldacap, oldhmask, newhmask;
  int32_t moveint;
  TValue *oldnumslot;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 8, 8);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  moveint = (int32_t)oldasize + 5;

  lj_tab_storeint(L, lj_tab_setint(L, t, moveint), 707);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldnumslot = find_num_slot(oldnode, oldhmask, moveint);
  assert(oldnumslot != NULL);

  lj_tab_resize(L, t, (uint32_t)oldasize + 16u, lj_fls(oldhmask) + 1u);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert_i32(lj_tab_getint(t, moveint), 707);

  store_forward(oldnumslot);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  assert_i32(lj_tab_getint(t, moveint), 707);
  lj_tab_storeint(L, lj_tab_setint(L, t, moveint), 909);
  assert_forward(oldnumslot);
  assert_i32(&newarray[moveint], 909);
  assert_i32(lj_tab_getint(t, moveint), 909);
  assert(count_next(t) == 1);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  GCstr *visible, *hidden;
  TValue *slot;

  assert(L != NULL);
  lua_createtable(L, 4, 4);
  t = tabV(L->top-1);

  lj_tab_storeint(L, lj_tab_setint(L, t, 1), 11);
  lj_tab_storeint(L, lj_tab_setint(L, t, 2), 22);
  slot = lj_tab_setint(L, t, 3);
  lj_tab_storeint(L, slot, 33);
  assert(lj_tab_len(t) == 3);
  store_forward(slot);
  assert(lj_tab_getint(t, 3) == NULL);
  assert(lj_tab_len(t) == 2);
#if LJ_HASJIT
  assert(lj_tab_len_hint(t, 2) == 2);
#endif

  visible = newstr(L, "tab-forward-filter-visible");
  hidden = newstr(L, "tab-forward-filter-hidden");
  lj_tab_storeint(L, lj_tab_setstr(L, t, visible), 44);
  slot = lj_tab_setstr(L, t, hidden);
  lj_tab_storeint(L, slot, 55);
  store_forward(slot);
  assert(lj_tab_getstr(t, hidden) == NULL);
  {
    TValue key;
    setstrV(L, &key, hidden);
    assert(tvisnil(lj_tab_get(L, t, &key)));
  }

  assert(count_next(t) == 3);
  assert(lj_tab_len(t) == 2);
  exercise_array_forward_hop(L);
  exercise_hash_forward_hop(L);
  exercise_hash_to_array_forward_hop(L);

  lua_close(L);
  printf("t-tab-forward-filter OK: FORWARD values stay internal to table scans\n");
  return 0;
}
