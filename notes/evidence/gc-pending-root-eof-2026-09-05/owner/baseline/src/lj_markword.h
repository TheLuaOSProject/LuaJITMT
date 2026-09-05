/*
** lj_markword.h - epoch-tagged concurrent bitmap word.
**
** A complete {bits, epoch} value is changed with CX16.  This makes lazy epoch
** reset a single linearizable operation: a stale marker can neither resurrect
** an old epoch nor overwrite bits published by another marker in the current
** epoch.  The enclosing bitmap supplies one word per 64 arena cells.
*/
#ifndef _LJ_MARKWORD_H
#define _LJ_MARKWORD_H

#include "lj_atomic.h"

#if !defined(__x86_64__)
#error "GC2 epoch markwords currently require the x86-64 CX16 contract"
#endif

#define LJ_ARENA_MARK_EPOCH_NONE UINT64_C(0)

typedef struct LJArenaMarkWord {
  la_u128 value;  /* lo = bits, hi = epoch. */
} LJArenaMarkWord;

typedef struct LJArenaMarkSnap {
  uint64_t bits;
  uint64_t epoch;
} LJArenaMarkSnap;

typedef enum LJArenaMarkResult {
  LJ_ARENA_MARK_INVALID = -2,
  LJ_ARENA_MARK_STALE = -1,
  LJ_ARENA_MARK_UNCHANGED = 0,
  LJ_ARENA_MARK_CHANGED = 1
} LJArenaMarkResult;

typedef char lj_markword_size_must_be_16[
  sizeof(LJArenaMarkWord) == 16 ? 1 : -1];
typedef char lj_markword_align_must_be_16[
  __alignof__(LJArenaMarkWord) >= 16 ? 1 : -1];

/* Only valid before the containing arena is published. */
LA_INLINE int lj_markword_init_unpublished(LJArenaMarkWord *word,
                                            uint64_t epoch, uint64_t bits)
{
  if (epoch == LJ_ARENA_MARK_EPOCH_NONE && bits != 0)
    return 0;
  word->value.lo = bits;
  word->value.hi = epoch;
  return 1;
}

/*
** A writer changes both halves with CX16.  The epoch/bit/epoch sample avoids a
** 16-byte load (and therefore any compiler runtime fallback) on the hot read
** path.  Acquire ordering makes a matching epoch a visibility proof for the
** sampled bits.  This is an explicit x86-64 compiler contract: aligned 64-bit
** subloads must remain coherent with the enclosing cmpxchg16b modification.
** GCC, Clang, MinGW and Darwin artifact tests enforce that lowering.
*/
LA_INLINE LJArenaMarkSnap lj_markword_snapshot(const LJArenaMarkWord *word)
{
  LJArenaMarkSnap snap;
  uint64_t again;
  do {
    snap.epoch = la_load64_acq(&word->value.hi);
    snap.bits = la_load64_acq(&word->value.lo);
    again = la_load64_acq(&word->value.hi);
  } while (snap.epoch != again);
  return snap;
}

LA_INLINE int lj_markword_test(const LJArenaMarkWord *word, uint64_t epoch,
                               unsigned bit)
{
  LJArenaMarkSnap snap;
  if (epoch == LJ_ARENA_MARK_EPOCH_NONE || bit >= 64)
    return 0;
  snap = lj_markword_snapshot(word);
  return snap.epoch == epoch && (snap.bits & (UINT64_C(1) << bit)) != 0;
}

/*
** Publish one live/grey bit for epoch.  Moving to a newer epoch lazily drops
** all old bits in the same CX16 operation.  A request for an older epoch is
** rejected instead of being allowed to move the word backwards.
*/
LA_INLINE LJArenaMarkResult lj_markword_set(LJArenaMarkWord *word,
                                             uint64_t epoch, unsigned bit)
{
  uint64_t mask;
  if (epoch == LJ_ARENA_MARK_EPOCH_NONE || bit >= 64)
    return LJ_ARENA_MARK_INVALID;
  mask = UINT64_C(1) << bit;
  for (;;) {
    LJArenaMarkSnap snap = lj_markword_snapshot(word);
    la_u128 expected, desired;
    if (snap.epoch > epoch)
      return LJ_ARENA_MARK_STALE;
    if (snap.epoch == epoch && (snap.bits & mask) != 0)
      return LJ_ARENA_MARK_UNCHANGED;
    expected.lo = snap.bits;
    expected.hi = snap.epoch;
    desired.lo = snap.epoch == epoch ? snap.bits | mask : mask;
    desired.hi = epoch;
    if (la_cas128(&word->value, &expected, desired))
      return LJ_ARENA_MARK_CHANGED;
  }
}

/* Clear only an exact-epoch bit.  This never initializes a future epoch. */
LA_INLINE LJArenaMarkResult lj_markword_clear(LJArenaMarkWord *word,
                                               uint64_t epoch, unsigned bit)
{
  uint64_t mask;
  if (epoch == LJ_ARENA_MARK_EPOCH_NONE || bit >= 64)
    return LJ_ARENA_MARK_INVALID;
  mask = UINT64_C(1) << bit;
  for (;;) {
    LJArenaMarkSnap snap = lj_markword_snapshot(word);
    la_u128 expected, desired;
    if (snap.epoch > epoch)
      return LJ_ARENA_MARK_STALE;
    if (snap.epoch < epoch || (snap.bits & mask) == 0)
      return LJ_ARENA_MARK_UNCHANGED;
    expected.lo = snap.bits;
    expected.hi = snap.epoch;
    desired.lo = snap.bits & ~mask;
    desired.hi = epoch;
    if (la_cas128(&word->value, &expected, desired))
      return LJ_ARENA_MARK_CHANGED;
  }
}

#endif /* _LJ_MARKWORD_H */
