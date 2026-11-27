#ifndef TEST_GC_PENDING_REQUEST_H
#define TEST_GC_PENDING_REQUEST_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lj_gc2.h"

/* String allocation requests a cycle at its accounting threshold. A real C
** frame keeps the outer C API string checkpoint from driving that request
** before the fixture can race STOP or RESTART with a native attachment. */
static int request_gc_through_strings(lua_State *L)
{
  global_State *g = G(L);
  uint32_t i;
  for (i = 0; i < 8192 && gc2_cycle_leader_acq(g) == 0; i++) {
    char buf[65];
    int n = snprintf(buf, sizeof(buf), "stop-overlap:%08x:", i);
    assert(n > 0 && n < 64);
    memset(buf + n, 'x', 64u - (unsigned)n);
    lua_pushlstring(L, buf, 64);
    lua_pop(L, 1);
  }
  return 0;
}

#endif
