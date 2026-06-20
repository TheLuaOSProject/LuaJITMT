/*
** Focused guard for M5 table array publication and SMR retirement.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_tab.h"

static TabArrayRetire *find_retired_array(global_State *g, TValue *array)
{
  TabArrayRetire *ret;
  for (ret = (TabArrayRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_arrays);
       ret != NULL;
       ret = lj_tab_array_retired_next_acq(ret))
    if (ret->array == array)
      return ret;
  return NULL;
}

static void set_int(lua_State *L, GCtab *t, int32_t k, int32_t v)
{
  setintV(lj_tab_setint(L, t, k), v);
}

static int32_t get_int(GCtab *t, int32_t k)
{
  cTValue *slot = lj_tab_getint(t, k);
  TValue tv;
  assert(slot != NULL);
  lj_tv_load_acq(&tv, slot);
  assert(tvisnumber(&tv));
  return tvisint(&tv) ? intV(&tv) : (int32_t)numberVnum(&tv);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *t;
  TValue *oldarray, *array;
  TValue *snaparray;
  MSize oldasize, oldacap;
  MSize snapasize;
  uint64_t retire_epoch;
  TabArrayRetire *ret;
  int i;

  assert(L != NULL);
  g = G(L);
  assert(la_loadptr_acq((void *const *)&g->tab.retired_arrays) == NULL);

  lua_createtable(L, LJ_MAX_COLOSIZE + 8, 0);
  t = tabV(L->top-1);
  assert(t->colo == 0);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert(oldarray != NULL);
  assert(oldasize == oldacap);
  assert(lj_tab_array_mem_acq(t) == lj_tab_array_hdrw(oldarray));
  assert(lj_tab_array_hdr_asize_acq(oldarray) == oldasize);
  assert(lj_tab_array_hdr_acap_acq(oldarray) == oldacap);
  assert(lj_tab_array_hdr_flags_acq(oldarray) == 0);
  assert(lj_tab_array_nextgen_acq(oldarray) == NULL);
  assert(!lj_tab_array_is_retiring(t, oldarray));
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == oldarray);
  assert(snapasize == oldasize);
  for (i = 0; i < (int)oldasize; i += 3)
    set_int(L, t, i, i + 1000);

  lj_tab_resize(L, t, oldasize + 32, 0);
  array = lj_tab_array_acq(t);
  assert(array != oldarray);
  assert(lj_tab_asize_acq(t) == oldasize + 32);
  assert(t->acap == oldasize + 32);
  assert(lj_tab_array_mem_acq(t) == lj_tab_array_hdrw(array));
  assert(lj_tab_array_hdr_asize_acq(array) == oldasize + 32);
  assert(lj_tab_array_hdr_acap_acq(array) == oldasize + 32);
  assert(lj_tab_array_hdr_flags_acq(array) == 0);
  assert(lj_tab_array_nextgen_acq(array) == NULL);
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == array);
  assert(snapasize == oldasize + 32);
  ret = find_retired_array(g, oldarray);
  assert(ret != NULL);
  assert(ret->acap == oldacap);
  assert(lj_tab_array_hdr_acap_acq(ret->array) == ret->acap);
  assert(lj_tab_array_hdr_flags_acq(ret->array) == TABARRAY_FLAG_RETIRING);
  assert(lj_tab_array_nextgen_acq(ret->array) == array);
  assert(lj_tab_array_is_retiring(t, ret->array));
  assert(ret->armed == 1);
  retire_epoch = ret->retire_epoch;
  assert(lj_tab_reclaim_retired(g, retire_epoch) == 0);
  assert(find_retired_array(g, oldarray) != NULL);
  assert(lj_tab_reclaim_retired(g, retire_epoch + 1u) == 1);
  assert(find_retired_array(g, oldarray) == NULL);
  for (i = 0; i < (int)oldasize; i += 3)
    assert(get_int(t, i) == i + 1000);
  for (i = (int)oldasize; i < (int)lj_tab_asize_acq(t); i++)
    assert(lj_tv_isnil_acq(&array[i]));

  array = lj_tab_array_acq(t);
  oldacap = t->acap;
  set_int(L, t, 9, 9009);
  set_int(L, t, (int32_t)oldasize + 4, 4444);
  oldarray = array;
  lj_tab_resize(L, t, 6, 4);
  array = lj_tab_array_acq(t);
  assert(array != oldarray);
  assert(t->acap == oldacap);
  assert(lj_tab_asize_acq(t) == 6);
  assert(lj_tab_array_hdr_asize_acq(array) == 6);
  assert(lj_tab_array_hdr_acap_acq(array) == oldacap);
  assert(lj_tab_array_hdr_flags_acq(array) == 0);
  assert(lj_tab_array_nextgen_acq(array) == NULL);
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == array);
  assert(snapasize == 6);
  ret = find_retired_array(g, oldarray);
  assert(ret != NULL);
  assert(ret->acap == oldacap);
  assert(lj_tab_array_hdr_flags_acq(ret->array) == TABARRAY_FLAG_RETIRING);
  assert(lj_tab_array_nextgen_acq(ret->array) == array);
  assert(lj_tab_array_is_retiring(t, ret->array));
  retire_epoch = ret->retire_epoch;
  assert(lj_tab_reclaim_retired(g, retire_epoch + 1u) >= 1);
  assert(find_retired_array(g, oldarray) == NULL);
  assert(get_int(t, 9) == 9009);
  assert(get_int(t, (int32_t)oldasize + 4) == 4444);
  assert(lj_tv_isnil_acq(&array[9]));
  assert(lj_tv_isnil_acq(&array[oldasize + 4]));

  oldarray = array;
  oldacap = t->acap;
  lj_tab_resize(L, t, 12, 4);
  array = lj_tab_array_acq(t);
  assert(array != oldarray);
  assert(t->acap == 12);
  assert(lj_tab_asize_acq(t) == 12);
  assert(lj_tab_array_hdr_asize_acq(array) == 12);
  assert(lj_tab_array_hdr_acap_acq(array) == 12);
  assert(lj_tab_array_hdr_flags_acq(array) == 0);
  assert(lj_tab_array_nextgen_acq(array) == NULL);
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == array);
  assert(snapasize == 12);
  ret = find_retired_array(g, oldarray);
  assert(ret != NULL);
  assert(ret->acap == oldacap);
  assert(lj_tab_array_hdr_flags_acq(ret->array) == TABARRAY_FLAG_RETIRING);
  assert(lj_tab_array_nextgen_acq(ret->array) == array);
  assert(lj_tab_array_is_retiring(t, ret->array));
  retire_epoch = ret->retire_epoch;
  assert(lj_tab_reclaim_retired(g, retire_epoch + 1u) >= 1);
  assert(find_retired_array(g, oldarray) == NULL);
  assert(get_int(t, 9) == 9009);
  lua_pop(L, 1);

  lua_createtable(L, 3, 0);
  t = tabV(L->top-1);
  assert(t->colo > 0);
  oldarray = lj_tab_array_acq(t);
  oldacap = t->acap;
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == oldarray);
  assert(snapasize == lj_tab_asize_acq(t));
  set_int(L, t, 1, 111);
  lj_tab_resize(L, t, LJ_MAX_COLOSIZE + 24, 0);
  assert(t->colo < 0);
  assert(t->acap == LJ_MAX_COLOSIZE + 24);
  array = lj_tab_array_acq(t);
  assert(array != oldarray);
  assert(lj_tab_array_mem_acq(t) == lj_tab_array_hdrw(array));
  assert(lj_tab_array_hdr_asize_acq(array) == LJ_MAX_COLOSIZE + 24);
  assert(lj_tab_array_hdr_acap_acq(array) == LJ_MAX_COLOSIZE + 24);
  assert(lj_tab_array_hdr_flags_acq(array) == 0);
  assert(lj_tab_array_nextgen_acq(array) == NULL);
  snapasize = lj_tab_array_snapshot_acq(t, &snaparray);
  assert(snaparray == array);
  assert(snapasize == LJ_MAX_COLOSIZE + 24);
  assert(find_retired_array(g, oldarray) == NULL);
  assert(get_int(t, 1) == 111);
  assert(oldacap <= LJ_MAX_COLOSIZE);

  lua_close(L);
  printf("t-tab-array-publish OK: arrays publish ordered and retire by epoch\n");
  return 0;
}
