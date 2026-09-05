/*
** Shared helpers for C tests that exercise table FORWARD-slot behavior.
*/

#ifndef TESTS_LIB_TAB_FORWARD_HELPERS_H
#define TESTS_LIB_TAB_FORWARD_HELPERS_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

static LJ_AINLINE void tabfwd_store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static LJ_AINLINE void tabfwd_assert_forward(cTValue *tv)
{
  TValue val;
  lj_tv_load_acq(&val, tv);
  assert(tvisforward(&val));
}

static LJ_AINLINE int32_t tabfwd_tv_i32(cTValue *tv)
{
  assert(tv != NULL);
  assert(tvisnumber(tv));
  return tvisint(tv) ? intV(tv) : (int32_t)numV(tv);
}

static LJ_AINLINE void tabfwd_assert_i32(cTValue *tv, int32_t want)
{
  assert(tabfwd_tv_i32(tv) == want);
}

static LJ_AINLINE void tabfwd_set_int(lua_State *L, GCtab *t,
				      int32_t k, int32_t v)
{
  lj_tab_storeint(L, lj_tab_setint(L, t, k), v);
}

static LJ_AINLINE int32_t tabfwd_get_i32(GCtab *t, int32_t k)
{
  return tabfwd_tv_i32(lj_tab_getint(t, k));
}

static LJ_AINLINE GCstr *tabfwd_newstr(lua_State *L, const char *s)
{
  return lj_str_new(L, s, strlen(s));
}

static LJ_AINLINE GCstr *tabfwd_find_sid_bucket(lua_State *L,
						const char *prefix,
						uint32_t mask,
						uint32_t bucket,
						uint32_t *seq)
{
  for (;;) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s-%u-%08x",
		     prefix, bucket, (*seq)++);
    GCstr *s;
    assert(n > 0 && n < (int)sizeof(buf));
    s = tabfwd_newstr(L, buf);
    if (((uint32_t)s->sid & mask) == bucket)
      return s;
  }
}

static LJ_AINLINE void tabfwd_set_str_i32(lua_State *L, GCtab *t,
					  GCstr *s, int32_t v)
{
  lj_tab_storeint(L, lj_tab_setstr(L, t, s), v);
}

static LJ_AINLINE void tabfwd_assert_str_i32(GCtab *t, GCstr *s, int32_t want)
{
  cTValue *tv = lj_tab_getstr(t, s);
  assert(tv != NULL);
  tabfwd_assert_i32(tv, want);
}

static LJ_AINLINE void tabfwd_set_cstr_i32(lua_State *L, GCtab *t,
					   const char *key, int32_t v)
{
  tabfwd_set_str_i32(L, t, tabfwd_newstr(L, key), v);
}

static LJ_AINLINE void tabfwd_assert_cstr_i32(lua_State *L, GCtab *t,
					      const char *key, int32_t want)
{
  tabfwd_assert_str_i32(t, tabfwd_newstr(L, key), want);
}

static LJ_AINLINE int tabfwd_count_next_visible(GCtab *t)
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

static LJ_AINLINE Node *tabfwd_find_str_node(Node *node, MSize hmask,
					     const GCstr *key)
{
  Node *n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key)
      return n;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

static LJ_AINLINE TValue *tabfwd_find_str_slot(Node *node, MSize hmask,
					       const GCstr *key)
{
  Node *n = tabfwd_find_str_node(node, hmask, key);
  return n ? &n->val : NULL;
}

static LJ_AINLINE Node *tabfwd_find_num_node(Node *node, MSize hmask,
					     cTValue *key)
{
  Node *n = hashnum_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == key->n)
      return n;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

static LJ_AINLINE TValue *tabfwd_find_num_slot_tv(Node *node, MSize hmask,
						  cTValue *key)
{
  Node *n = tabfwd_find_num_node(node, hmask, key);
  return n ? &n->val : NULL;
}

static LJ_AINLINE TValue *tabfwd_find_num_slot(Node *node, MSize hmask,
					       int32_t key)
{
  TValue k;
  setnumV(&k, (lua_Number)key);
  return tabfwd_find_num_slot_tv(node, hmask, &k);
}

static LJ_AINLINE TValue *tabfwd_find_key_slot(Node *node, MSize hmask,
					       cTValue *key)
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

static LJ_AINLINE void tabfwd_load_lua(lua_State *L, const char *src)
{
  int status = luaL_loadstring(L, src);
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "luaL_loadstring failed");
  }
  assert(status == 0);
}

static LJ_AINLINE void tabfwd_run_loaded(lua_State *L)
{
  int status = lua_pcall(L, 0, 0, 0);
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "lua_pcall failed");
  }
  assert(status == 0);
}

#endif
