#ifndef DENSE_HELPERS_H
#define DENSE_HELPERS_H
#include "lj_arena.h"
/* Test-only namespace compression, used while the fixture owns/stops actors. */
static inline void dense_seed(const void *p, uint64_t era, uint32_t serial,
                       uint32_t cycle, int wide)
{
  LJGC2TabStamp *s = lj_arena_gc2_stamp_acq(p);
  LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(p);
  la_u128 old, next;
  assert(s && w);
  old = lj_arena_gc2_wide_snapshot(w);
  next.lo = ((uint64_t)cycle << 32) | serial;
  next.hi = era;
  assert(la_cas128(&w->proof, &old, next));
  la_store64_rel(&s->state, wide ? UINT32_MAX :
    ((uint64_t)cycle << 32) | serial);
}
static inline la_u128 dense_snapshot(const void *p)
{
  return lj_arena_gc2_wide_snapshot(lj_arena_gc2_wide_acq(p));
}
#endif

