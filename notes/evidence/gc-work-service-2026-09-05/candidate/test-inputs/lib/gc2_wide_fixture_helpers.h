#ifndef LJT_GC2_WIDE_FIXTURE_HELPERS_H
#define LJT_GC2_WIDE_FIXTURE_HELPERS_H
#include "lj_arena.h"
/* Test-only namespace compression. Retain storage authority and exclude
** concurrent proof updates through private ownership or a paused test schedule.
** A sampled address or stamp alone is not permission to read a body. */
static inline void ljt_gc2_wide_seed(const void *p, uint64_t era,
                                     uint32_t serial, uint32_t cycle, int wide)
{
  LJGC2TabStamp *s = lj_arena_gc2_stamp_acq(p);
  LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(p);
  la_u128 old, next;
  assert(s && w);
  old = lj_arena_gc2_wide_snapshot(w);
  next.lo = ((uint64_t)cycle << 32) | serial;
  next.hi = era;
  assert(la_cas128(&w->proof, &old, next));
  la_store64_rel(&s->state,
                 wide ? UINT32_MAX : ((uint64_t)cycle << 32) | serial);
}
static inline la_u128 ljt_gc2_wide_snapshot(const void *p)
{
  return lj_arena_gc2_wide_snapshot(lj_arena_gc2_wide_acq(p));
}
#endif
