/* GCobj is a common view; only concrete tables require 16-byte alignment. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lj_obj.h"

LJ_STATIC_ASSERT(_Alignof(GCobj) == 8);
LJ_STATIC_ASSERT(_Alignof(GCtab) >= 16);
LJ_STATIC_ASSERT((offsetof(GCtab, struct_weak_pair) & 15u) == 0);

int main(void)
{
  void *storage = calloc(1, sizeof(GCobj)+16u);
  GCobj *tabstorage = (GCobj *)calloc(1, sizeof(GCobj));
  GCobj *o = (GCobj *)((char *)storage+8u);
  GCstr *str = (GCstr *)o;
  GCtab *table = (GCtab *)tabstorage;
  GCRef ref;
  TValue value;
  assert(storage && tabstorage);
  assert(((uintptr_t)o & 15u) == 8u);
  assert(((uintptr_t)&table->struct_weak_pair & 15u) == 0);
  str->gct = (uint8_t)~LJ_TSTR;
  table->gct = (uint8_t)~LJ_TTAB;
  setgcref(ref, o);
  assert(gcref(ref) == o);
  setgcVraw(&value, o, LJ_TSTR);
  assert(tvisstr(&value) && strV(&value) == str);
  lj_obj_setgcw(o, obj2gco(table));
  assert(lj_obj_gcw(o) == obj2gco(table));
  assert(gco2str(o) == str);
  assert(gco2tab(obj2gco(table)) == table);
  free(tabstorage);
  free(storage);
  puts("t-gcobj-alignment OK: 8-byte generic view and 16-byte table pair");
  return 0;
}
