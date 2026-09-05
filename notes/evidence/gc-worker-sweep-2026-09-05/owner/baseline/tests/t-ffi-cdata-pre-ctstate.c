/*
** Fixed predefined cdata must validate before CTState publication.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cdata.h"

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  global_State *g;
  GCcdata *cd;
  void *base = NULL;
  GCSize size = 0;
  uint16_t flags, id;
  uint8_t marked;

  assert(L != NULL);
  g = G(L);
  assert(ctype_ctsG(g) == NULL);

  /* lj_cdata_new_ links the object immediately; GC root validation therefore
  ** exercises the bootstrap layout oracle before this function returns. */
  cd = lj_cdata_new_(L, CTID_INT32, 4);
  assert(cd != NULL);
  assert(lj_cdata_validate(g, cd, &base, &size) == 1);
  assert(base == cd);
  assert(size == sizeof(GCcdata) + 4u);
  setcdataV(L, L->top, cd);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;

  flags = cdata_flags_acq(cd);
  cdata_flags_rel(cd, (uint16_t)(flags ^ LJ_CDATA_SIZE_TAIL_MASK));
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  cdata_flags_rel(cd, flags);

  id = cd->ctypeid;
  cd->ctypeid = (uint16_t)(CTID_CTYPEID + 1u);
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  cd->ctypeid = id;

  marked = la_load8_acq(&cd->marked);
  la_store8_rel(&cd->marked, (uint8_t)(marked | 0x80u));
  assert(lj_cdata_validate(g, cd, NULL, NULL) == 0);
  la_store8_rel(&cd->marked, marked);

  assert(lj_cdata_validate(g, cd, &base, &size) == 1);
  lua_close(L);
  printf("t-ffi-cdata-pre-ctstate OK: fixed bootstrap layout validated\n");
#else
  printf("t-ffi-cdata-pre-ctstate SKIP: FFI disabled\n");
#endif
  return 0;
}
