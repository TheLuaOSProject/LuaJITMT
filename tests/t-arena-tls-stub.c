/* Minimal TLS query for standalone allocator fixtures.
** Full-VM fixtures link the real implementation from lj_thr.c. */

#include "lj_def.h"

typedef struct TGState TGState;

LJ_FUNC TGState *lj_thr_get_tg(void)
{
  return NULL;
}
