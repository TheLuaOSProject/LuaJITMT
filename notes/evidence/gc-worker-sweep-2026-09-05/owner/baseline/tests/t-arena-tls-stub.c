/* Minimal TLS query for standalone allocator fixtures.
** Full-VM fixtures link the real implementation from lj_thr.c. */

#include "lj_def.h"

typedef struct TGState TGState;
typedef struct global_State global_State;

LJ_FUNC TGState *lj_thr_get_tg(void)
{
  return NULL;
}

LJ_FUNC void lj_gc2_sweep_publish_wake(global_State *g)
{
  UNUSED(g);
}
