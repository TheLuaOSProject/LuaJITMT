/*
** Table handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_tab_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"

/* -- Object hashing ------------------------------------------------------ */

/* Hash an arbitrary key against a previously acquired hash vector snapshot. */
static Node *hashkey_node(Node *node, MSize hmask, cTValue *key)
{
  lj_assertX(!tvisint(key), "attempt to hash integer");
  if (tvisstr(key))
    return hashstr_node(node, hmask, strV(key));
  else if (tvisnum(key))
    return hashnum_node(node, hmask, key);
  else if (tvisbool(key))
    return hashmask_node(node, hmask, boolV(key));
  else
    return hashgcref_node(node, hmask, key->gcr);
}

static LJ_AINLINE int tab_nextnode_cas(Node *n, Node **expect, Node *want)
{
#if LJ_GC64
  uint64_t old = (uint64_t)(uintptr_t)(*expect);
  int ok = la_cas64(&n->next.ptr64, &old, (uint64_t)(uintptr_t)want,
		    LA_ACQ_REL, LA_ACQ);
  if (!ok)
    *expect = (Node *)(void *)(uintptr_t)old;
  return ok;
#else
  uint32_t old = (uint32_t)(uintptr_t)(*expect);
  int ok = la_cas32(&n->next.ptr32, &old, (uint32_t)(uintptr_t)want,
		    LA_ACQ_REL, LA_ACQ);
  if (!ok)
    *expect = (Node *)(void *)(uintptr_t)old;
  return ok;
#endif
}

static LJ_AINLINE int tab_val_isclaim(cTValue *tv, cTValue *claim)
{
  return tv_rawload(tv) == tv_rawload(claim);
}

static LJ_AINLINE int tab_key_islocked(cTValue *key)
{
  return tviskeylock(key);
}

static LJ_AINLINE int tab_key_retry_once(cTValue *key, int *retry)
{
  if (tab_key_islocked(key) && *retry) {
    *retry = 0;
    la_cpu_pause();
    return 1;
  }
  return 0;
}

static LJ_AINLINE int tab_val_absent(cTValue *val)
{
  return tvisnil(val) || tvisforward(val);
}

static LJ_AINLINE int tab_slot_absent_acq(const TValue *slot)
{
  TValue val;
  lj_tv_load_acq(&val, slot);
  return tab_val_absent(&val);
}

static LJ_AINLINE int tab_val_forward_retry_once(cTValue *val, int *retry)
{
  if (tvisforward(val) && *retry) {
    *retry = 0;
    la_cpu_pause();
    return 1;
  }
  return 0;
}

static LJ_AINLINE int tab_node_forward_hop(Node **nodep, MSize *hmaskp)
{
  Node *node = *nodep;
  Node *next = lj_tab_node_nextgen_acq(node);
  if (next && next != node) {
    *nodep = next;
    *hmaskp = lj_tab_node_hmask_acq(next);
    return 1;
  }
  return 0;
}

static LJ_AINLINE TValue *tab_forwarded_int_arrayslot(GCtab *t, int32_t key)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  if ((MSize)key < asize)
    return &array[key];
  if (lj_tab_array_forward_hop(t, &array, &asize) && (MSize)key < asize)
    return &array[key];
  return NULL;
}

static LJ_AINLINE int tab_forwarded_hash_value(GCtab *t, Node **nodep,
					       MSize *hmaskp, cTValue *key,
					       TValue *valp)
{
  Node *n;
  if (!tab_node_forward_hop(nodep, hmaskp))
    return 0;
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      cTValue *tv = lj_tab_getint(t, k);
      if (tv) {
	lj_tv_load_acq(valp, tv);
	return !tab_val_absent(valp);
      }
    }
  }
  n = hashkey_node(*nodep, *hmaskp, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key)) {
      lj_tv_load_acq(valp, &n->val);
      return !tab_val_absent(valp);
    }
  } while ((n = lj_tab_nextnode_acq(n)));
  return 0;
}

static TValue *tab_forwarded_setslot(GCtab *t, Node **nodep, MSize *hmaskp,
				     cTValue *key)
{
  Node *n;
  if (!tab_node_forward_hop(nodep, hmaskp))
    return NULL;
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      TValue *slot = tab_forwarded_int_arrayslot(t, k);
      if (slot)
	return slot;
    }
  }
  if (*hmaskp == 0)
    return NULL;
  n = hashkey_node(*nodep, *hmaskp, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return &n->val;
    if (tab_key_islocked(&nk))
      return NULL;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

static LJ_AINLINE int tab_array_slot_absent_acq(GCtab *t, TValue **arrayp,
						MSize *asizep, MSize idx)
{
  TValue val;
  TValue *array = *arrayp;
  lj_tv_load_acq(&val, &array[idx]);
  if (tvisforward(&val)) {
    MSize nextasize = *asizep;
    TValue *nextarray = array;
    if (lj_tab_array_forward_hop(t, &nextarray, &nextasize) &&
	idx < nextasize) {
      *arrayp = nextarray;
      *asizep = nextasize;
      lj_tv_load_acq(&val, &nextarray[idx]);
    }
  }
  return tab_val_absent(&val);
}

static TValue *tab_findkey_or_keylock(Node *anchor, cTValue *key, int *locked)
{
  Node *n;
  *locked = 0;
  for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return &n->val;
    if (tab_key_islocked(&nk)) {
      *locked = 1;
      return NULL;
    }
  }
  return NULL;
}

/* -- Table creation and destruction -------------------------------------- */

static LJ_AINLINE Node *tab_node_new(lua_State *L, MSize hmask)
{
  TabNodeHdr *hdr = (TabNodeHdr *)lj_mem_new(L, lj_tab_node_bytes(hmask));
  Node *node = (Node *)(void *)((char *)(void *)hdr + sizeof(TabNodeHdr));
  hdr->hmask = hmask;
  hdr->flags = 0;
  setmref(hdr->next_gen, NULL);
#if !LJ_GC64
  hdr->reserved = 0;
#endif
  return node;
}

static LJ_AINLINE void tab_node_free(global_State *g, Node *node, MSize hmask)
{
  lj_mem_free(g, lj_tab_node_hdrw(node), lj_tab_node_bytes(hmask));
}

static LJ_AINLINE TValue *tab_array_new(lua_State *L, MSize asize, MSize acap)
{
  TabArrayHdr *hdr = (TabArrayHdr *)lj_mem_new(L, lj_tab_array_bytes(acap));
  lj_tab_array_hdr_init(hdr, asize, acap);
  return lj_tab_array_slots(hdr);
}

static LJ_AINLINE void tab_array_free(global_State *g, TValue *array, MSize acap)
{
  lj_mem_free(g, lj_tab_array_hdrw(array), lj_tab_array_bytes(acap));
}

static LJ_AINLINE Node *newhpart_alloc(lua_State *L, uint32_t hbits,
				       MSize *hmaskp)
{
  uint32_t i, hsize, hmask;
  Node *node;
  lj_assertL(hbits != 0, "zero hash size");
  if (hbits > LJ_MAX_HBITS)
    lj_err_msg(L, LJ_ERR_TABOV);
  hsize = 1u << hbits;
  hmask = hsize - 1u;
  node = tab_node_new(L, hmask);
  for (i = 0; i < hsize; i++) {
    Node *n = &node[i];
    lj_tab_nextnode_set(n, NULL);
    lj_tab_storenilraw(&n->key);
    lj_tab_storenilraw(&n->val);
  }
  *hmaskp = hmask;
  return node;
}

static LJ_AINLINE void newhpart_publish(GCtab *t, Node *node, MSize hmask,
					Node *freetop)
{
  setfreetop(t, node, freetop);
  lj_tab_node_rel(t, node);
  lj_tab_hmask_rel(t, hmask);
}

/* Create new hash part for table. */
static LJ_AINLINE void newhpart(lua_State *L, GCtab *t, uint32_t hbits)
{
  MSize hmask;
  Node *node = newhpart_alloc(L, hbits, &hmask);
  newhpart_publish(t, node, hmask, &node[hmask+1]);
}

static LJ_AINLINE void tab_storekeyrel(lua_State *L, TValue *dst,
				       cTValue *key)
{
  TValue k;
  copyTV(L, &k, key);
  if (LJ_UNLIKELY(tvismzero(&k)))
    k.u64 = 0;
  copyTVrel(L, dst, &k);
}

static LJ_AINLINE void tab_storekeylockrel(TValue *dst)
{
  TValue keylock;
  setkeylockV(&keylock);
  tv_rawstore_rel(dst, tv_rawload(&keylock));
}

static TValue *tab_rehash_insert(lua_State *L, Node *nodebase, MSize hmask,
				 Node **freetopp, cTValue *key)
{
  /* Destination belongs to an unpublished replacement hash vector. */
  Node *n = hashkey_node(nodebase, hmask, key);
  if (!lj_tv_isnil_acq(&n->val)) {
    Node *freenode = *freetopp;
    do {
      lj_assertL(freenode > nodebase, "no free node during rehash");
    } while (!lj_tv_isnil_acq(&(--freenode)->key));
    *freetopp = freenode;
    lj_tab_nextnode_set(freenode, lj_tab_nextnode_acq(n));
    tab_storekeyrel(L, &freenode->key, key);
    lj_tab_nextnode_set(n, freenode);
    return &freenode->val;
  }
  tab_storekeyrel(L, &n->key, key);
  return &n->val;
}

static int tab_rehash_arrayindex(uint32_t asize, cTValue *key, uint32_t *idxp)
{
  int64_t i64;
  int32_t k;
  if (tvisint(key)) {
    k = intV(key);
  } else if (tvisnum(key) && lj_num2int_check(numV(key), i64, k)) {
    /* Numeric key converted below. */
  } else {
    return 0;
  }
  if ((MSize)k < asize) {
    *idxp = (uint32_t)k;
    return 1;
  }
  return 0;
}

static TValue *tab_rehash_arrayslot(TValue *array, uint32_t asize,
				    cTValue *key)
{
  uint32_t idx;
  return tab_rehash_arrayindex(asize, key, &idx) ? &array[idx] : NULL;
}

static TValue *tab_rehash_slot(lua_State *L, TValue *array, uint32_t asize,
			       Node *nodebase, MSize hmask, Node **freetopp,
			       cTValue *key)
{
  TValue *slot = tab_rehash_arrayslot(array, asize, key);
  return slot ? slot : tab_rehash_insert(L, nodebase, hmask, freetopp, key);
}

static uint32_t tab_rehash_hashcount(Node *oldnode, MSize oldhmask,
				     TValue *oldarray, uint32_t oldasize,
				     uint32_t asize)
{
  uint32_t count = 0;
  if (oldhmask > 0) {
    uint32_t i;
    for (i = 0; i <= oldhmask; i++) {
      Node *n = &oldnode[i];
      TValue key, val;
      lj_tv_load_acq(&val, &n->val);
      if (!tab_val_absent(&val)) {
	uint32_t idx;
	lj_tv_load_acq(&key, &n->key);
	if (!tab_rehash_arrayindex(asize, &key, &idx))
	  count++;
      }
    }
  }
  if (asize < oldasize) {
    uint32_t i;
    for (i = asize; i < oldasize; i++)
      if (!tab_slot_absent_acq(&oldarray[i]))
	count++;
  }
  return count;
}

static void tab_retired_push(global_State *g, TabNodeRetire *ret)
{
  void *head = la_loadptr_acq((void *const *)&g->tab.retired_nodes);
  do {
    ret->next = (TabNodeRetire *)head;
  } while (!la_casptr((void **)&g->tab.retired_nodes, &head, ret,
		      LA_ACQ_REL, LA_ACQ));  /* 06 section 6.3.5 raw retire. */
}

static TabNodeRetire *tab_retire_reserve(lua_State *L, Node *node,
					 MSize hmask)
{
  TabNodeRetire *ret = lj_mem_newt(L, sizeof(TabNodeRetire), TabNodeRetire);
  ret->node = node;
  ret->hmask = hmask;
  la_store64_rel(&ret->retire_epoch, 0);
  la_store32_rel(&ret->armed, 0);
  ret->next = NULL;
  tab_retired_push(G(L), ret);
  return ret;
}

static void tab_retire_arm(global_State *g, TabNodeRetire *ret)
{
  la_store64_rel(&ret->retire_epoch, la_load64_acq(&g->gc2.hs_epoch));
  la_store32_rel(&ret->armed, 1);
}

static void tab_array_retired_push(global_State *g, TabArrayRetire *ret)
{
  void *head = la_loadptr_acq((void *const *)&g->tab.retired_arrays);
  do {
    ret->next = (TabArrayRetire *)head;
  } while (!la_casptr((void **)&g->tab.retired_arrays, &head, ret,
			      LA_ACQ_REL, LA_ACQ));  /* 06 section 6.3.1 raw retire. */
}

static TabArrayRetire *tab_array_retire_reserve(lua_State *L, TValue *array,
						MSize acap)
{
  TabArrayRetire *ret = lj_mem_newt(L, sizeof(TabArrayRetire), TabArrayRetire);
  ret->array = array;
  ret->acap = acap;
  lj_assertL(!array || acap == lj_tab_array_hdr_acap_acq(array),
	     "mismatched retired table array capacity");
  la_store64_rel(&ret->retire_epoch, 0);
  la_store32_rel(&ret->armed, 0);
  ret->next = NULL;
  tab_array_retired_push(G(L), ret);
  return ret;
}

static void tab_array_retire_arm(global_State *g, TabArrayRetire *ret)
{
  la_store64_rel(&ret->retire_epoch, la_load64_acq(&g->gc2.hs_epoch));
  la_store32_rel(&ret->armed, 1);
}

/*
** Q: Why all of these copies of t->hmask, t->node etc. to local variables?
** A: Because alias analysis for C is _really_ tough.
**    Even state-of-the-art C compilers won't produce good code without this.
*/

/* Clear hash part of table. */
static LJ_AINLINE void clearhpart(GCtab *t)
{
  Node *node = lj_tab_node_acq(t);
  uint32_t i, hmask = lj_tab_node_hmask_acq(node);
  lj_assertX(hmask != 0, "empty hash part");
  for (i = 0; i <= hmask; i++) {
    Node *n = &node[i];
    lj_tab_nextnode_rel(n, NULL);
    lj_tab_storenilraw(&n->val);
    lj_tab_storenilraw(&n->key);
  }
}

/* Clear array part of table. */
static LJ_AINLINE void clearapart(GCtab *t)
{
  TValue *array;
  uint32_t i, asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  for (i = 0; i < asize; i++)
    lj_tab_storenilraw(&array[i]);
}

/* Create a new table. Note: the slots are not initialized (yet). */
static GCtab *newtab(lua_State *L, uint32_t asize, uint32_t hbits)
{
  GCtab *t;
  /* First try to colocate the array part. */
  if (LJ_MAX_COLOSIZE != 0 && asize > 0 && asize <= LJ_MAX_COLOSIZE) {
    Node *nilnode;
    lj_assertL((sizeof(GCtab) & 7) == 0, "bad GCtab size");
    t = (GCtab *)lj_mem_newgco(L, sizetabcolo(asize));
    t->gct = ~LJ_TTAB;
    t->nomm = (uint8_t)~0;
    t->colo = (int8_t)asize;
    lj_tab_array_set(t, (TValue *)((char *)t + sizeof(GCtab)));
    setgcrefnull(t->metatable);
    t->asize = asize;
    t->acap = asize;
    t->hmask = 0;
    nilnode = &G(L)->nilnode;
    lj_tab_node_set(t, nilnode);
#if LJ_GC64
    setmref(t->freetop, nilnode);
#endif
  } else {  /* Otherwise separately allocate the array part. */
    Node *nilnode;
    t = lj_mem_newobj(L, GCtab);
    t->gct = ~LJ_TTAB;
    t->nomm = (uint8_t)~0;
    t->colo = 0;
    lj_tab_array_set(t, NULL);
    setgcrefnull(t->metatable);
    t->asize = 0;  /* In case the array allocation fails. */
    t->acap = 0;
    t->hmask = 0;
    nilnode = &G(L)->nilnode;
    lj_tab_node_set(t, nilnode);
#if LJ_GC64
    setmref(t->freetop, nilnode);
#endif
    if (asize > 0) {
      if (asize > LJ_MAX_ASIZE)
	lj_err_msg(L, LJ_ERR_TABOV);
      lj_tab_array_set(t, tab_array_new(L, asize, asize));
      t->asize = asize;
      t->acap = asize;
    }
  }
  if (hbits)
    newhpart(L, t, hbits);
  return t;
}

/* Create a new table.
**
** IMPORTANT NOTE: The API differs from lua_createtable()!
**
** The array size is non-inclusive. E.g. asize=128 creates array slots
** for 0..127, but not for 128. If you need slots 1..128, pass asize=129
** (slot 0 is wasted in this case).
**
** The hash size is given in hash bits. hbits=0 means no hash part.
** hbits=1 creates 2 hash slots, hbits=2 creates 4 hash slots and so on.
*/
GCtab *lj_tab_new(lua_State *L, uint32_t asize, uint32_t hbits)
{
  GCtab *t = newtab(L, asize, hbits);
  clearapart(t);
  return t;
}

/* The API of this function conforms to lua_createtable(). */
GCtab *lj_tab_new_ah(lua_State *L, uint32_t a, uint32_t h)
{
  return lj_tab_new(L, a ? a+1 : 0, hsize2hbits(h));
}

#if LJ_HASJIT
GCtab * LJ_FASTCALL lj_tab_new1(lua_State *L, uint32_t ahsize)
{
  GCtab *t = newtab(L, ahsize & 0xffffff, ahsize >> 24);
  clearapart(t);
  return t;
}
#endif

/* Duplicate a table. */
GCtab * LJ_FASTCALL lj_tab_dup(lua_State *L, const GCtab *kt)
{
  GCtab *t;
  TValue *karray;
  uint32_t asize, hmask;
  MSize khmask;
  Node *knode = lj_tab_node_snapshot_acq(kt, &khmask);
  hmask = (uint32_t)khmask;
  asize = (uint32_t)lj_tab_array_snapshot_acq(kt, &karray);
  t = newtab(L, asize, hmask > 0 ? lj_fls(hmask)+1 : 0);
  asize = (uint32_t)lj_tab_array_snapshot_acq(kt, &karray);
  lj_assertL(asize == lj_tab_asize_acq(t) &&
	     hmask == lj_tab_node_hmask_acq(lj_tab_node_acq(t)),
	     "mismatched size of table and template");
  t->nomm = 0;  /* Keys with metamethod names may be present. */
  if (asize > 0) {
    TValue *array = lj_tab_array_acq(t);
    uint32_t i;
    for (i = 0; i < asize; i++)
      lj_tv_load_acq(&array[i], &karray[i]);
  }
  if (hmask > 0) {
    uint32_t i;
    Node *node = lj_tab_node_acq(t);
    ptrdiff_t d = (char *)node - (char *)knode;
    setfreetop(t, node, (Node *)((char *)getfreetop(kt, knode) + d));
    for (i = 0; i <= hmask; i++) {
      Node *kn = &knode[i];
      Node *n = &node[i];
      TValue key, val;
      Node *next = lj_tab_nextnode_acq(kn);
      /* Don't use copyTV here, since it asserts on a copy of a dead key. */
      lj_tv_load_acq(&val, &kn->val);
      lj_tv_load_acq(&key, &kn->key);
      /* Private duplicate table construction; not shared publication. */
      n->val = val; n->key = key;
      if (tvistab(&n->val))  /* Replace nil value marker. */
	lj_tab_storenilraw(&n->val);
      lj_tab_nextnode_set(n, next == NULL ? next : (Node *)((char *)next + d));
    }
  }
  return t;
}

/* Clear a table. */
void LJ_FASTCALL lj_tab_clear(GCtab *t)
{
  Node *node;
  MSize hmask;
  clearapart(t);
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);
  if (hmask > 0) {
    setfreetop(t, node, &node[hmask+1]);
    clearhpart(t);
  }
}

/* Free a table. */
void LJ_FASTCALL lj_tab_free(global_State *g, GCtab *t)
{
  MSize size = LJ_MAX_COLOSIZE != 0 && t->colo ?
	       sizetabcolo((uint32_t)t->colo & 0x7f) : sizeof(GCtab);
  Node *node = lj_tab_node_acq(t);
  MSize hmask = lj_tab_node_hmask_acq(node);
  if (hmask > 0)
    tab_node_free(g, node, hmask);
  if (t->acap > 0 && lj_tab_array_separated(t))
    tab_array_free(g, lj_tab_array_acq(t), t->acap);
  if (!lj_mem_freegco_defer(g, t, size))
    lj_mem_free(g, t, size);
}

/* -- Table resizing ------------------------------------------------------ */

/* Resize a table to fit the new array/hash part sizes. */
void lj_tab_resize(lua_State *L, GCtab *t, uint32_t asize, uint32_t hbits)
{
  Node *oldnode = lj_tab_node_acq(t);
  TValue *oldarray = lj_tab_array_acq(t);
  uint32_t oldasize = lj_tab_asize_acq(t);
  uint32_t oldacap = t->acap;
  uint32_t oldhmask = lj_tab_node_hmask_acq(oldnode);
  TValue *array = oldarray;
  uint32_t newacap = oldacap;
  int array_changed = asize != oldasize;
  int newarray = 0;
  MSize newhmask = 0;
  Node *newnode = NULL;
  Node *newfreetop = NULL;
  uint32_t hashcount = tab_rehash_hashcount(oldnode, oldhmask, oldarray,
					    oldasize, asize);
  TabNodeRetire *oldret = oldhmask > 0 ?
    tab_retire_reserve(L, oldnode, oldhmask) : NULL;
  TabArrayRetire *oldaret = NULL;
  if (hashcount) {
    uint32_t needhbits = hsize2hbits(hashcount);
    if (hbits < needhbits)
      hbits = needhbits;
  }
  if (asize > oldasize && asize > LJ_MAX_ASIZE)
    lj_err_msg(L, LJ_ERR_TABOV);
  if (array_changed) {
    if (lj_tab_array_separated(t)) {
      newarray = 1;
      newacap = asize >= oldasize ? asize : oldacap;
    } else if (asize > oldacap) {
      newarray = 1;
      newacap = asize;
    }
  }
  if (newarray) {
    uint32_t i;
    uint32_t copy = oldasize < newacap ? oldasize : newacap;
    if (lj_tab_array_separated(t) && oldacap > 0)
      oldaret = tab_array_retire_reserve(L, oldarray, oldacap);
    array = tab_array_new(L, asize, newacap);
    for (i = 0; i < copy; i++)
      lj_tv_load_acq(&array[i], &oldarray[i]);
    for (i = asize; i < copy; i++)
      lj_tab_storenilraw(&array[i]);
    if (LJ_MAX_COLOSIZE != 0 && t->colo > 0)
      t->colo = (int8_t)(t->colo | 0x80);  /* Mark as separated (colo < 0). */
    t->acap = newacap;
  }
  if (asize > oldasize) {  /* Array part grows? */
    uint32_t i;
    for (i = oldasize; i < asize; i++)  /* Clear newly allocated slots. */
      lj_tab_storenilraw(&array[i]);
  }
  if (hbits) {
    newnode = newhpart_alloc(L, hbits, &newhmask);
    newfreetop = &newnode[newhmask+1];
  }
  if (oldhmask > 0) {  /* Reinsert pairs from old hash part. */
    uint32_t i;
    for (i = 0; i <= oldhmask; i++) {
      Node *n = &oldnode[i];
      TValue key, val;
      lj_tv_load_acq(&val, &n->val);
      if (!tab_val_absent(&val)) {
	TValue *slot;
	lj_tv_load_acq(&key, &n->key);
	if (hbits) {
	  slot = tab_rehash_slot(L, array, asize, newnode, newhmask,
				 &newfreetop, &key);
	} else {
	  slot = tab_rehash_arrayslot(array, asize, &key);
	  lj_assertL(slot != NULL, "missing hash part during rehash");
	}
	copyTVrel(L, slot, &val);
      }
    }
  }
  if (hbits && asize < oldasize) {  /* Reinsert old array tail off-table. */
    uint32_t i;
    for (i = asize; i < oldasize; i++) {
      TValue key, val;
      lj_tv_load_acq(&val, &oldarray[i]);
      if (!tab_val_absent(&val)) {
	setnumV(&key, (lua_Number)i);
	copyTVrel(L, tab_rehash_insert(L, newnode, newhmask, &newfreetop, &key),
		  &val);
      }
    }
  }
  if (oldaret) {
    lj_tab_array_nextgen_rel(oldarray, array);
    lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  }
  if (asize > oldasize) {
    if (array != oldarray)
      lj_tab_array_rel(t, array);
    lj_tab_asize_rel(t, asize);
    if (oldaret)
      tab_array_retire_arm(G(L), oldaret);
  }
  if (oldret) {
    lj_tab_node_nextgen_rel(oldnode, hbits ? newnode : &G(L)->nilnode);
    lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  }
  /* Publish the rebuilt hash part. */
  if (hbits) {
    newhpart_publish(t, newnode, newhmask, newfreetop);
    if (oldret)
      tab_retire_arm(G(L), oldret);
  } else {
    global_State *g = G(L);
    lj_tab_hmask_rel(t, 0);
#if LJ_GC64
    setmref(t->freetop, &g->nilnode);
#endif
    lj_tab_node_rel(t, &g->nilnode);
    if (oldret) {
      tab_retire_arm(g, oldret);
    }
  }
  if (asize < oldasize) {  /* Array part shrinks? */
    lj_tab_asize_rel(t, asize);  /* This 'shrinks' even colocated arrays. */
    if (array != oldarray)
      lj_tab_array_rel(t, array);
    if (oldaret)
      tab_array_retire_arm(G(L), oldaret);
  }
  if (oldhmask > 0)
    lj_assertL(oldret && la_load32_acq(&oldret->armed),
	       "retired table nodes not armed");
}

uint32_t lj_tab_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  ret = (TabNodeRetire *)la_xchgptr_acqrel((void **)&g->tab.retired_nodes,
					   NULL);
  while (ret) {
    TabNodeRetire *next = ret->next;
    ret->next = NULL;
    if (!la_load32_acq(&ret->armed)) {
      tab_retired_push(g, ret);
    } else if (la_load64_acq(&ret->retire_epoch) < completed_epoch) {
      tab_node_free(g, ret->node, ret->hmask);
      lj_mem_freet(g, ret);
      reclaimed++;
    } else {
      tab_retired_push(g, ret);
    }
    ret = next;
  }
  aret = (TabArrayRetire *)la_xchgptr_acqrel(
    (void **)&g->tab.retired_arrays, NULL);
  while (aret) {
    TabArrayRetire *next = aret->next;
    aret->next = NULL;
    if (!la_load32_acq(&aret->armed)) {
      tab_array_retired_push(g, aret);
    } else if (la_load64_acq(&aret->retire_epoch) < completed_epoch) {
      tab_array_free(g, aret->array, aret->acap);
      lj_mem_freet(g, aret);
      reclaimed++;
    } else {
      tab_array_retired_push(g, aret);
    }
    aret = next;
  }
  return reclaimed;
}

void lj_tab_freeretired(global_State *g)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  if (!g)
    return;
  ret = (TabNodeRetire *)la_xchgptr_acqrel((void **)&g->tab.retired_nodes,
					   NULL);
  while (ret) {
    TabNodeRetire *next = ret->next;
    if (la_load32_acq(&ret->armed))
      tab_node_free(g, ret->node, ret->hmask);
    lj_mem_freet(g, ret);
    ret = next;
  }
  aret = (TabArrayRetire *)la_xchgptr_acqrel(
    (void **)&g->tab.retired_arrays, NULL);
  while (aret) {
    TabArrayRetire *next = aret->next;
    if (la_load32_acq(&aret->armed))
      tab_array_free(g, aret->array, aret->acap);
    lj_mem_freet(g, aret);
    aret = next;
  }
}

static uint32_t countint(cTValue *key, uint32_t *bins)
{
  lj_assertX(!tvisint(key), "bad integer key");
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_cond(numV(key), i64, k, (uint32_t)i64 < LJ_MAX_ASIZE)) {
      bins[(k > 2 ? lj_fls((uint32_t)(k-1)) : 0)]++;
      return 1;
    }
  }
  return 0;
}

static uint32_t countarray(const GCtab *t, uint32_t *bins)
{
  TValue *array;
  uint32_t na, b, i, asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  if (asize == 0) return 0;
  for (na = i = b = 0; b < LJ_MAX_ABITS; b++) {
    uint32_t n, top = 2u << b;
    if (top >= asize) {
      top = asize-1;
      if (i > top)
	break;
    }
    for (n = 0; i <= top; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (!tab_val_absent(&val))
	n++;
    }
    bins[b] += n;
    na += n;
  }
  return na;
}

static uint32_t counthash(const GCtab *t, uint32_t *bins, uint32_t *narray)
{
  MSize hmask;
  Node *node = lj_tab_node_snapshot_acq(t, &hmask);
  uint32_t total, na, i;
  for (total = na = 0, i = 0; i <= hmask; i++) {
    Node *n = &node[i];
    TValue key, val;
    lj_tv_load_acq(&val, &n->val);
    if (!tab_val_absent(&val)) {
      lj_tv_load_acq(&key, &n->key);
      na += countint(&key, bins);
      total++;
    }
  }
  *narray += na;
  return total;
}

static uint32_t bestasize(uint32_t bins[], uint32_t *narray)
{
  uint32_t b, sum, na = 0, sz = 0, nn = *narray;
  for (b = 0, sum = 0; 2*nn > (1u<<b) && sum != nn; b++)
    if (bins[b] > 0 && 2*(sum += bins[b]) > (1u<<b)) {
      sz = (2u<<b)+1;
      na = sum;
    }
  *narray = sz;
  return na;
}

static void rehashtab(lua_State *L, GCtab *t, cTValue *ek)
{
  uint32_t bins[LJ_MAX_ABITS];
  uint32_t total, asize, na, i;
  for (i = 0; i < LJ_MAX_ABITS; i++) bins[i] = 0;
  asize = countarray(t, bins);
  total = 1 + asize;
  total += counthash(t, bins, &asize);
  asize += countint(ek, bins);
  na = bestasize(bins, &asize);
  total -= na;
  lj_tab_resize(L, t, asize, hsize2hbits(total));
}

void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize)
{
  MSize hmask;
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  lj_tab_resize(L, t, nasize+1, hmask > 0 ? lj_fls(hmask)+1 : 0);
}

/* -- Table getters ------------------------------------------------------- */

cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1, forward_retry = 1;
  k.n = (lua_Number)key;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0)
    return NULL;
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(&node, &hmask)) {
	cTValue *tv = tab_forwarded_int_arrayslot(t, key);
	if (tv)
	  return tv;
	goto genlookup;
      }
      if (tab_val_forward_retry_once(&val, &forward_retry))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

cTValue * LJ_FASTCALL lj_tab_getint_hop(GCtab *t, int32_t key)
{
  return lj_tab_getint(t, key);
}

cTValue *lj_tab_getstr(GCtab *t, const GCstr *key)
{
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1, forward_retry = 1;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0)
    return NULL;
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(&node, &hmask))
	goto genlookup;
      if (tab_val_forward_retry_once(&val, &forward_retry))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key)
{
  int key_retry = 1, forward_retry = 1;
  if (tvisstr(key)) {
    cTValue *tv = lj_tab_getstr(t, strV(key));
    if (tv)
      return tv;
  } else if (tvisint(key)) {
    cTValue *tv = lj_tab_getint(t, intV(key));
    if (tv)
      return tv;
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      cTValue *tv = lj_tab_getint(t, k);
      if (tv)
	return tv;
    } else {
      goto retry_lookup;  /* Else use the generic lookup. */
    }
  } else if (!tvisnil(key)) {
    Node *node;
    MSize hmask;
    Node *n;
  retry_lookup:
    node = lj_tab_node_snapshot_acq(t, &hmask);
  genlookup:
    if (hmask == 0)
      return niltv(L);
    n = hashkey_node(node, hmask, key);
    do {
      TValue nk;
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	TValue val;
	lj_tv_load_acq(&val, &n->val);
	if (tvisforward(&val) && tab_node_forward_hop(&node, &hmask))
	  goto genlookup;
	if (tab_val_forward_retry_once(&val, &forward_retry))
	  goto retry_lookup;
	if (tvisforward(&val))
	  return niltv(L);
	return &n->val;
      }
      if (tab_key_retry_once(&nk, &key_retry))
	goto retry_lookup;
    } while ((n = lj_tab_nextnode_acq(n)));
  }
  return niltv(L);
}

/* -- Table setters ------------------------------------------------------- */

/* Insert new key. Nodes are never moved within a hash generation. */
TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key)
{
  Node *nodebase;
  MSize hmask;
  Node *n;
retry_insert:
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0) {
    rehashtab(L, t, key);
    return lj_tab_set(L, t, key);
  }
  n = hashkey_node(nodebase, hmask, key);
  {
    int locked;
    TValue *slot = tab_findkey_or_keylock(n, key, &locked);
    if (slot)
      return slot;
    if (locked) {
      la_cpu_pause();
      goto retry_insert;
    }
  }
  if (!lj_tv_isnil_acq(&n->val)) {
    Node *freenode = getfreetop(t, nodebase);
    lj_assertL(nodebase != &G(L)->nilnode, "insert into fallback hash");
    lj_assertL(freenode >= nodebase && freenode <= nodebase+hmask+1,
	       "bad freenode");
    do {
      if (freenode == nodebase) {  /* No free node found? */
	rehashtab(L, t, key);  /* Rehash table. */
	return lj_tab_set(L, t, key);  /* Retry key insertion. */
      }
      --freenode;
      {
	TValue fk;
	lj_tv_load_acq(&fk, &freenode->key);
	if (tab_key_islocked(&fk)) {
	  la_cpu_pause();
	  goto retry_insert;
	}
	if (tvisnil(&fk))
	  break;
      }
    } while (1);
    {
      int locked;
      TValue *slot = tab_findkey_or_keylock(n, key, &locked);
      if (slot)
	return slot;
      if (locked) {
	la_cpu_pause();
	goto retry_insert;
      }
    }
    setfreetop(t, nodebase, freenode);
    lj_assertL(freenode != &G(L)->nilnode, "store to fallback hash");
    lj_tab_nextnode_set(freenode, lj_tab_nextnode_acq(n));
    tab_storekeyrel(L, &freenode->key, key);
    lj_gc2_barrier_weak_key(L, t, key);
    lj_gc_pubtab(L, t);
    lj_assertL(lj_tv_isnil_acq(&freenode->val),
	       "new hash slot is not empty");
    lj_tab_nextnode_rel(n, freenode);
    return &freenode->val;
  }
  tab_storekeyrel(L, &n->key, key);
  lj_gc2_barrier_weak_key(L, t, key);
  lj_gc_pubtab(L, t);
  lj_assertL(lj_tv_isnil_acq(&n->val), "new hash slot is not empty");
  return &n->val;
}

int lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,
			     cTValue *claim, TValue **slot)
{
  MSize hmask;
  Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  Node *n;
  if (hmask == 0)
    return 0;
  n = hashkey_node(nodebase, hmask, key);
  for (;;) {
    TValue nk, nv, expect;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return -1;  /* A racing inserter published the key; retry lookup. */
    if (tviskeylock(&nk)) {
      la_cpu_pause();  /* Claimed empty anchor is publishing its key. */
      continue;
    }
    if (!tvisnil(&nk))
      return 0;  /* Caller handles collision-chain or resize fallback. */
    lj_tv_load_acq(&nv, &n->val);
    if (!tvisnil(&nv)) {
      la_cpu_pause();  /* Another claimed empty anchor is publishing key. */
      continue;
    }
    setnilV(&expect);
    if (lj_tv_cas(&n->val, &expect, claim)) {
      tab_storekeylockrel(&n->key);
      tab_storekeyrel(L, &n->key, key);
      *slot = &n->val;
      return 1;
    }
  }
}

static void tab_release_claimed_free(Node *n)
{
  lj_tab_nextnode_set(n, NULL);
  lj_tab_storenilraw(&n->key);
  lj_tab_storenilraw(&n->val);
}

int lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,
			    cTValue *claim, TValue **slot)
{
  MSize hmask;
  Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  Node *anchor, *reserved = NULL;
  if (hmask == 0)
    return 0;
  anchor = hashkey_node(nodebase, hmask, key);
  for (;;) {
    Node *n;
    MSize i;
    for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
      TValue nk, nv;
      lj_tv_load_acq(&nv, &n->val);
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	if (reserved)
	  tab_release_claimed_free(reserved);
	return -1;  /* Existing or racing insert for this key; retry lookup. */
      }
      if (tviskeylock(&nk) || (tvisnil(&nk) && tab_val_isclaim(&nv, claim))) {
	la_cpu_pause();  /* Linked collision insert has not published key. */
	goto retry;
      }
    }
    if (!reserved) {
      for (i = 0; i <= hmask; i++) {
	TValue nk, nv, expect;
	n = &nodebase[i];
	if (n == anchor)
	  continue;
	lj_tv_load_acq(&nk, &n->key);
	if (lj_obj_equal(&nk, key))
	  return -1;
	if (tviskeylock(&nk)) {
	  la_cpu_pause();  /* Unlinked free-node key claim is publishing. */
	  goto retry;
	}
	if (!tvisnil(&nk))
	  continue;
	lj_tv_load_acq(&nv, &n->val);
	if (!tvisnil(&nv)) {
	  if (tab_val_isclaim(&nv, claim)) {
	    la_cpu_pause();  /* Unlinked free-node claim is still publishing. */
	    goto retry;
	  }
	  continue;
	}
	setnilV(&expect);
	if (lj_tv_cas(&n->val, &expect, claim)) {
	  tab_storekeylockrel(&n->key);
	  reserved = n;  /* Claimed free node; not visible until CAS-prepend. */
	  break;
	}
      }
      if (!reserved)
	return 0;  /* No free node in this hash generation: resize fallback. */
      continue;  /* Re-scan chain before publishing the claimed node. */
    }
    if (LJ_UNLIKELY(anchor == NULL)) {
      tab_release_claimed_free(reserved);
      return 0;
    }
    n = lj_tab_nextnode_acq(anchor);
    lj_tab_nextnode_set(reserved, n);
    if (tab_nextnode_cas(anchor, &n, reserved)) {
      tab_storekeyrel(L, &reserved->key, key);
      *slot = &reserved->val;
      return 1;  /* 11.4 FINREG collision insert CAS-prepend. */
    }
  retry:
    continue;
  }
}

TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1, forward_retry = 1;
  k.n = (lua_Number)key;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0)
    return lj_tab_newkey(L, t, &k);
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	TValue *slot = tab_forwarded_setslot(t, &node, &hmask, &k);
	if (slot)
	  return slot;
	if (tab_val_forward_retry_once(&val, &forward_retry))
	  goto retry_lookup;
      }
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return lj_tab_newkey(L, t, &k);
}

TValue *lj_tab_setstr(lua_State *L, GCtab *t, const GCstr *key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1, forward_retry = 1;
  setstrV(L, &k, key);
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0)
    return lj_tab_newkey(L, t, &k);
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	TValue *slot = tab_forwarded_setslot(t, &node, &hmask, &k);
	if (slot)
	  return slot;
	if (tab_val_forward_retry_once(&val, &forward_retry))
	  goto retry_lookup;
      }
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return lj_tab_newkey(L, t, &k);
}

TValue *lj_tab_set(lua_State *L, GCtab *t, cTValue *key)
{
  Node *n;
  t->nomm = 0;  /* Invalidate negative metamethod cache. */
  if (tvisstr(key)) {
    return lj_tab_setstr(L, t, strV(key));
  } else if (tvisint(key)) {
    return lj_tab_setint(L, t, intV(key));
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k))
      return lj_tab_setint(L, t, k);
    if (tvisnan(key))
      lj_err_msg(L, LJ_ERR_NANIDX);
    /* Else use the generic lookup. */
  } else if (tvisnil(key)) {
    lj_err_msg(L, LJ_ERR_NILIDX);
  }
  {
    Node *node;
    MSize hmask;
    int key_retry = 1, forward_retry = 1;
  retry_lookup:
    node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask != 0) {
      n = hashkey_node(node, hmask, key);
      do {
	TValue nk;
	lj_tv_load_acq(&nk, &n->key);
	if (lj_obj_equal(&nk, key)) {
	  TValue val;
	  lj_tv_load_acq(&val, &n->val);
	  if (tvisforward(&val)) {
	    TValue *slot = tab_forwarded_setslot(t, &node, &hmask, key);
	    if (slot)
	      return slot;
	    if (tab_val_forward_retry_once(&val, &forward_retry))
	      goto retry_lookup;
	  }
	  return &n->val;
	}
	if (tab_key_retry_once(&nk, &key_retry))
	  goto retry_lookup;
      } while ((n = lj_tab_nextnode_acq(n)));
    }
  }
  return lj_tab_newkey(L, t, key);
}

LJ_FUNCA TValue *lj_tab_storetv(lua_State *L, TValue *dst, cTValue *src)
{
  copyTVrel(L, dst, src);
  return dst;
}

static LJ_AINLINE int tab_ptr_index(uintptr_t base, uintptr_t elem,
				    size_t elemsz, MSize count, MSize *idx)
{
  uintptr_t end = base + (uintptr_t)count * elemsz;
  if (elem >= base && elem < end && (elem - base) % elemsz == 0) {
    *idx = (MSize)((elem - base) / elemsz);
    return 1;
  }
  return 0;
}

static TValue *tab_forwarded_jit_array_slot(lua_State *L, GCtab *parent,
					    TValue *dst)
{
  TValue val, *array;
  MSize asize, idx;
  lj_tv_load_acq(&val, dst);
  if (!tvisforward(&val))
    return dst;
  asize = lj_tab_array_snapshot_acq(parent, &array);
  if (tab_ptr_index((uintptr_t)array, (uintptr_t)dst, sizeof(TValue),
		    asize, &idx) &&
      lj_tab_array_forward_hop(parent, &array, &asize)) {
    if (idx < asize)
      return &array[idx];
    return lj_tab_setinth(L, parent, (int32_t)idx);
  }
  return dst;
}

static TValue *tab_forwarded_jit_hash_slot(GCtab *parent, TValue *dst,
					   TValue *keycopy, cTValue **keyp)
{
  TValue val;
  Node *node, *n;
  MSize hmask, idx;
  lj_tv_load_acq(&val, dst);
  if (!tvisforward(&val))
    return dst;
  node = lj_tab_node_snapshot_acq(parent, &hmask);
  if (!tab_ptr_index((uintptr_t)node, (uintptr_t)dst, sizeof(Node),
		     hmask + 1u, &idx))
    return dst;
  n = &node[idx];
  lj_tv_load_acq(keycopy, &n->key);
  if (tab_key_islocked(keycopy) || tvisnil(keycopy))
    return dst;
  *keyp = keycopy;
  {
    TValue *slot = tab_forwarded_setslot(parent, &node, &hmask, keycopy);
    return slot ? slot : dst;
  }
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent,
					     TValue *dst, cTValue *src)
{
  dst = tab_forwarded_jit_array_slot(L, parent, dst);
  copyTVrel(L, dst, src);
  lj_gc2_barrier_weak_write(L, parent, NULL, dst);  /* M8: traced weak-value array write. */
  lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src)
{
  Node *n = (Node *)dst;  /* Node.val is the first field. */
  TValue keycopy;
  cTValue *key = &n->key;
  dst = tab_forwarded_jit_hash_slot(parent, dst, &keycopy, &key);
  copyTVrel(L, dst, src);
  lj_gc2_barrier_weak_write(L, parent, key, dst);  /* M8: traced weak hash write. */
  lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src)
{
  copyTVrel(L, dst, src);
  lj_gc2_barrier_weak_write(L, parent, NULL, dst);  /* M8: traced numeric NEWREF write. */
  lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetvn(lua_State *L, TValue *dst, cTValue *src,
				 uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++)
    copyTVrel(L, &dst[i], &src[i]);
  return dst;
}

TValue *lj_tab_storenilraw(TValue *dst)
{
  TValue tv;
  setnilV(&tv);
  tv_rawstore_rel(dst, tv_rawload(&tv));
  return dst;
}

TValue *lj_tab_storenil(lua_State *L, TValue *dst)
{
  UNUSED(L);
  return lj_tab_storenilraw(dst);
}

TValue *lj_tab_storebool(lua_State *L, TValue *dst, int b)
{
  TValue tv;
  setboolV(&tv, b);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeint(lua_State *L, TValue *dst, int32_t i)
{
  TValue tv;
  setintV(&tv, i);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeintptr(lua_State *L, TValue *dst, intptr_t i)
{
  TValue tv;
  setintptrV(&tv, i);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storestr(lua_State *L, TValue *dst, GCstr *s)
{
  TValue tv;
  setstrV(L, &tv, s);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storetab(lua_State *L, TValue *dst, GCtab *tab)
{
  TValue tv;
  settabV(L, &tv, tab);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storethread(lua_State *L, TValue *dst, lua_State *th)
{
  TValue tv;
  setthreadV(L, &tv, th);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeproto(lua_State *L, TValue *dst, GCproto *pt)
{
  TValue tv;
  setprotoV(L, &tv, pt);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storefunc(lua_State *L, TValue *dst, GCfunc *fn)
{
  TValue tv;
  setfuncV(L, &tv, fn);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeudata(lua_State *L, TValue *dst, GCudata *ud)
{
  TValue tv;
  setudataV(L, &tv, ud);
  return lj_tab_storetv(L, dst, &tv);
}

/* -- Table traversal ----------------------------------------------------- */

/* Table traversal indexes:
**
** Array key index: [0 .. t->asize-1]
** Hash key index:  [t->asize .. t->asize+t->hmask]
** Invalid key:     ~0
*/

/* Get the successor traversal index of a key. */
uint32_t LJ_FASTCALL lj_tab_keyindex(GCtab *t, cTValue *key)
{
  TValue tmp;
  TValue *array;
  uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  UNUSED(array);
  if (tvisint(key)) {
    int32_t k = intV(key);
    if ((uint32_t)k < asize)
      return (uint32_t)k + 1;
    setnumV(&tmp, (lua_Number)k);
    key = &tmp;
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_cond(numV(key), i64, k, (uint32_t)i64 < asize))
      return (uint32_t)k + 1;
  }
  if (!tvisnil(key)) {
    Node *node;
    MSize hmask;
    Node *n;
    int retry = 1;
  retry_lookup:
    node = lj_tab_node_snapshot_acq(t, &hmask);
    n = hashkey_node(node, hmask, key);
    do {
      TValue nk;
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key))
	return asize + (uint32_t)((n+1) - node);
      if (tab_key_retry_once(&nk, &retry))
	goto retry_lookup;
    } while ((n = lj_tab_nextnode_acq(n)));
    if (key->u32.hi == LJ_KEYINDEX)  /* Despecialized ITERN while running. */
      return key->u32.lo;
    return ~0u;  /* Invalid key to next. */
  }
  return 0;  /* A nil key starts the traversal. */
}

/* Get the next key/value pair of a table traversal. */
int lj_tab_next(GCtab *t, cTValue *key, TValue *o)
{
  uint32_t idx = lj_tab_keyindex(t, key);  /* Find successor index of key. */
  TValue *array;
  uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  /* First traverse the array part. */
  for (; idx < asize; idx++) {
    TValue val;
    lj_tv_load_acq(&val, &array[idx]);
    if (tvisforward(&val)) {
      MSize nextasize = asize;
      TValue *nextarray = array;
      if (lj_tab_array_forward_hop(t, &nextarray, &nextasize)) {
	if (idx < nextasize) {
	  array = nextarray;
	  asize = (uint32_t)nextasize;
	  lj_tv_load_acq(&val, &array[idx]);
	} else {
	  cTValue *tv = lj_tab_getinth(t, (int32_t)idx);
	  if (tv)
	    lj_tv_load_acq(&val, tv);
	}
      }
    }
    if (LJ_LIKELY(!tab_val_absent(&val))) {
      setintV(o, idx);
      o[1] = val;
      return 1;
    }
  }
  idx -= asize;
  /* Then traverse the hash part. */
  {
    MSize hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    for (; idx <= hmask; idx++) {
      Node *n = &node[idx];
      TValue key, val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	lj_tv_load_acq(&key, &n->key);
	if (!tab_key_islocked(&key)) {
	  Node *hopnode = node;
	  MSize hophmask = hmask;
	  if (tab_forwarded_hash_value(t, &hopnode, &hophmask, &key, &val)) {
	    o[0] = key;
	    o[1] = val;
	    return 1;
	  }
	}
	continue;
      }
      if (!tab_val_absent(&val)) {
	lj_tv_load_acq(&key, &n->key);
	if (tab_key_islocked(&key)) {
	  la_cpu_pause();
	  lj_tv_load_acq(&val, &n->val);
	  lj_tv_load_acq(&key, &n->key);
	  if (tab_key_islocked(&key) || tab_val_absent(&val))
	    continue;
	}
	o[0] = key;
	o[1] = val;
	return 1;
      }
    }
  }
  return (int32_t)idx < 0 ? -1 : 0;  /* Invalid key or end of traversal. */
}

int32_t LJ_FASTCALL lj_tab_itern_forward(GCtab *t, uint32_t idx, TValue *ctrl)
{
  TValue key;
  int ok;
  key.u32.lo = idx;
  key.u32.hi = LJ_KEYINDEX;
  ok = lj_tab_next(t, &key, ctrl+1);
  if (ok == 1) {
    uint32_t next = lj_tab_keyindex(t, ctrl+1);
    ctrl->u32.lo = next;
    ctrl->u32.hi = LJ_KEYINDEX;
  }
  return ok;
}

int32_t LJ_FASTCALL lj_tab_vmnext_forward(GCtab *t, uint32_t idx, TValue *out)
{
  TValue key, kv[2];
  int ok;
  key.u32.lo = idx;
  key.u32.hi = LJ_KEYINDEX;
  ok = lj_tab_next(t, &key, kv);
  if (ok == 1) {
    out[0] = kv[1];
    out[1] = kv[0];
    return (int32_t)lj_tab_keyindex(t, &kv[0]);
  }
  setnilV(out+1);
  return ok < 0 ? -1 : 0;
}

/* -- Table length calculation -------------------------------------------- */

/* Compute table length. Slow path with mixed array/hash lookups. */
LJ_NOINLINE static MSize tab_len_slow(GCtab *t, size_t hi)
{
  cTValue *tv;
  size_t lo = hi;
  hi++;
  /* Widening search for an upper bound. */
  while ((tv = lj_tab_getint(t, (int32_t)hi)) && !tab_slot_absent_acq(tv)) {
    lo = hi;
    hi += hi;
    if (hi > (size_t)(0x7fffffff - 2)) {  /* Punt and do a linear search. */
      lo = 1;
      while ((tv = lj_tab_getint(t, (int32_t)lo)) && !tab_slot_absent_acq(tv))
	lo++;
      return (MSize)(lo - 1);
    }
  }
  /* Binary search to find a non-nil to nil transition. */
  while (hi - lo > 1) {
    size_t mid = (lo+hi) >> 1;
    cTValue *tvb = lj_tab_getint(t, (int32_t)mid);
    if (tvb && !tab_slot_absent_acq(tvb)) lo = mid; else hi = mid;
  }
  return (MSize)lo;
}

/* Compute table length. Fast path. */
MSize LJ_FASTCALL lj_tab_len(GCtab *t)
{
  TValue *array;
  MSize hmask;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  size_t hi = (size_t)asize;
  if (hi) hi--;
  /* In a growing array the last array element is very likely nil. */
  if (hi > 0 &&
      LJ_LIKELY(tab_array_slot_absent_acq(t, &array, &asize, (MSize)hi))) {
    /* Binary search to find a non-nil to nil transition in the array. */
    size_t lo = 0;
    while (hi - lo > 1) {
      size_t mid = (lo+hi) >> 1;
      if (tab_array_slot_absent_acq(t, &array, &asize, (MSize)mid))
	hi = mid;
      else
	lo = mid;
    }
    return (MSize)lo;
  }
  /* Without a hash part, there's an implicit nil after the last element. */
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  return hmask ? tab_len_slow(t, hi) : (MSize)hi;
}

#if LJ_HASJIT
/* Verify hinted table length or compute it. */
MSize LJ_FASTCALL lj_tab_len_hint(GCtab *t, size_t hint)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  if (LJ_LIKELY(hint+1 < asize)) {
    int absent = tab_array_slot_absent_acq(t, &array, &asize, (MSize)hint);
    int nextabsent = tab_array_slot_absent_acq(t, &array, &asize,
					       (MSize)(hint+1));
    if (LJ_LIKELY(!absent && nextabsent))
      return (MSize)hint;
  } else if (hint+1 <= asize) {
    MSize hmask;
    (void)lj_tab_node_snapshot_acq(t, &hmask);
    if (LJ_LIKELY(hmask == 0)) {
      if (!tab_array_slot_absent_acq(t, &array, &asize, (MSize)hint))
	return (MSize)hint;
    }
  }
  return lj_tab_len(t);
}
#endif
