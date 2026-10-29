/* Copy bytecode and its source shadow for private admission mutations.
** Other references retain their separately rooted original objects. */
#ifndef TESTS_PROTO_FIXTURE_HELPERS_H
#define TESTS_PROTO_FIXTURE_HELPERS_H

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lj_obj.h"

static LJ_AINLINE void ljt_proto_clone_reset(GCproto *copy, const GCproto *pt)
{
  size_t bcbytes = (size_t)pt->sizebc*sizeof(BCIns);
  assert(pt->sizept >= sizeof(GCproto)+2u*bcbytes);
  assert(mref(pt->jit_startins, const BCIns) == proto_bc(pt)+pt->sizebc);
  memcpy(copy, pt, pt->sizept);
  /* Production checks the co-located source shadow for patched entries. */
  setmref(copy->jit_startins, proto_bc(copy)+copy->sizebc);
}

static LJ_AINLINE GCproto *ljt_proto_clone_alloc(const GCproto *pt)
{
  GCproto *copy = (GCproto *)malloc(pt->sizept);
  assert(copy);
  ljt_proto_clone_reset(copy, pt);
  return copy;
}

#endif
