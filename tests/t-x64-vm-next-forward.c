/*
** Focused x64 guard for lj_vm_next over forwarded table slots.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_vm.h"

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static void set_int(lua_State *L, GCtab *t, int32_t k, int32_t v)
{
  lj_tab_storeint(L, lj_tab_setint(L, t, k), v);
}

static int32_t get_i32(GCtab *t, int32_t k)
{
  cTValue *tv = lj_tab_getint(t, k);
  assert(tv != NULL);
  assert(tvisnumber(tv));
  return tvisint(tv) ? intV(tv) : (int32_t)numV(tv);
}

static Node *find_str_node(Node *node, MSize hmask, const GCstr *key)
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

static uint32_t call_vm_next(GCtab *t, uint32_t idx, TValue *val, TValue *key)
{
  uint64_t valu, keyu;
  uint32_t next;
  __asm__(
    "subq $32, %%rsp\n\t"
    "movq %[tab], %%rdi\n\t"
    "movl %k[start], %%esi\n\t"
    "call lj_vm_next\n\t"
    "movq (%%rax), %[valu]\n\t"
    "movq 8(%%rax), %[keyu]\n\t"
    "movl %%edx, %[next]\n\t"
    "addq $32, %%rsp\n\t"
    : [valu] "=r"(valu), [keyu] "=r"(keyu), [next] "=r"(next)
    : [tab] "r"(t), [start] "r"(idx)
    : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11",
      "memory", "cc");
  val->u64 = valu;
  key->u64 = keyu;
  return next;
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
    set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }
  assert(get_i32(t, target) == target + 7100);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  store_forward(&oldarray[target]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  next = call_vm_next(t, (uint32_t)target, &val, &key);
  assert(next != (uint32_t)-1);
  assert(tvisnumber(&val));
  assert((tvisint(&val) ? intV(&val) : (int32_t)numV(&val)) ==
	 target + 7100);
  assert(!tvistabinternal(&val));
  assert(!tvistabinternal(&key));

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lua_pop(L, 1);
}

static void exercise_hash_forward(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  Node *oldnode, *newnode, *oldn;
  TValue val, key;
  MSize oldhmask, newhmask;
  uint32_t idx, next;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "vm_next_forward_field",
		    sizeof("vm_next_forward_field") - 1u);
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 8181);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);

  store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);
  idx = lj_tab_asize_acq(t) + (uint32_t)(oldn - oldnode);
  next = call_vm_next(t, idx, &val, &key);
  assert(next != (uint32_t)-1);
  assert(tvisnumber(&val));
  assert((tvisint(&val) ? intV(&val) : (int32_t)numV(&val)) == 8181);
  assert(tvisstr(&key) && strV(&key) == hkey);
  assert(!tvistabinternal(&val));
  assert(!tvistabinternal(&key));

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_array_forward(L);
  exercise_hash_forward(L);

  lua_close(L);
  printf("t-x64-vm-next-forward OK: lj_vm_next resolves forwarded slots\n");
  return 0;
}
