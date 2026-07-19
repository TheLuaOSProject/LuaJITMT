# JIT VM-event attachment-clock substrate (2026-07-19)

## Scope and current limitation

This tranche now includes dormant publication-clock storage, bounded readers,
exact registry-key-to-lane mapping and a no-cancel writer state machine. It
still does not change `jit.attach()`, handler replacement, handler lookup,
VM-event callback ownership, TRACE delivery, or callback behavior. No
production caller claims a writer yet, so every clock remains zero in ordinary
execution today.

In particular, the provisional nonzero attachment nonce accepted by the
structural TRACE FLUSH stream gate is not replaced by this tranche. A later
transaction must publish a real clock generation with the exact handler-table
mutation before stream delivery may use it as attachment identity.

`plan/` is unchanged.

## Append-only layout

`LJJitEventAttachmentClock` is the three-scalar descriptor anticipated by the
TRACE stream design:

```c
typedef struct LJJitEventAttachmentClock {
  uint64_t sequence;
  uint64_t next_generation;
  uint64_t generation;
} LJJitEventAttachmentClock;
```

Eight clocks, one for each low-three-bit VM-event/mask lane, are appended
after the already-published `jit_trace_stream` tail in `TGState`. Static
alignment, ordering, size and final-tail assertions keep every pre-existing
field, event-session field and stream-descriptor field at its old offset.

As with the global TRACE stream descriptor, only
`g->main_tg->jit_event_attachment` is authoritative. Every secondary TG copy
is zero-initialized alongside its event sessions solely to keep allocation and
bootstrap symmetric; snapshot helpers never consult it.

The deliberate temporary cost is 8 x 24 bytes, or 192 bytes, in every JIT
enabled TG even though secondary copies are unused. That is the simplest
append-only, ABI-safe substrate while TG construction remains uniform. A
future representation may centralize the clocks only if it can preserve all
already-published offsets and lifecycle rules; this tranche does not trade
layout safety for those 192 bytes.

## Stable shapes

Each clock has two accepted stable shapes:

- `INITIAL`: sequence, next generation and generation are all zero; or
- `PUBLISHED`: sequence is nonzero and even, generation is nonzero, and
  `next_generation == generation` exactly.

There is intentionally no stable `sequence != 0, generation == 0` idle shape.
Every successful writer claim must publish a nonzero generation, including a
conservative invalidation after it has claimed the clock but collides during
the exact handler-table store. There is no writer cancellation API and no
restore-the-old-even escape after claim. Consequently, advancing the stable
sequence without a generation would mean that the table/clock transaction was
only partly published and readers must refuse it rather than silently call a
mismatched handler.

Generation and sequence zero are never reused after a completed publication.
The final nonzero generation (`UINT64_MAX`) and final even sequence
(`UINT64_MAX-1`) remain readable canonical states. The writer refuses before
either counter would wrap.

## Exact event lanes and defined hashing

Clock selection starts from the final signed 32-bit integer key used by the
`_VMEVENTS` table, not from a string comparison. This preserves stock
hash-collision behavior: any event name which produces a known table key names
the corresponding known lane.

| Registry-key bits | Event | Lane |
| --- | --- | ---: |
| `0x0001c418` | BC | 0 |
| `0x96c8a338` | TRACE | 1 |
| `0x9425fa78` | RECORD | 2 |
| `0x94ef9580` | TEXIT | 3 |
| `0x96c9c440` | ERRFIN | 4 |

Unknown final keys return no lane and clear the output to `UINT32_MAX`. Lanes
5 through 7 remain reserved for future events. Mapping remains available in a
no-JIT build even though that build has no clock storage.

Both `VMEVENT_HASHIDX` and the event-enum constructor now shift an unsigned
32-bit value before explicitly converting the final bits to `int32_t`. The old
signed shifts overflowed for TRACE, RECORD, TEXIT and ERRFIN under the C
abstract machine even though the target compilers produced the intended x86
bits.

## Mandatory writer completion

`lj_jit_event_attachment_writer_claim()` reports four distinct results:

- `CLAIMED`: the exact stable even sequence was changed to odd and the next
  generation was reserved;
- `BUSY`: an odd writer, changing snapshot or losing sequence CAS was observed;
- `EXHAUSTED`: the stable canonical clock cannot advance both counters without
  wrapping; or
- `CORRUPT`: configuration, lane, main-TG or stable-shape authority is invalid.

Failed claims clear their handle. A successful claim records only the global,
old sequence, new generation and lane. It does not retain a secondary-TG or
raw clock pointer. `writer_publish()` rederives the authoritative main-TG
clock, fail-stops on an impossible handle/storage mismatch, release-publishes
`VMEVENT_NOCACHE`, then the reserved generation, and finally the exact matching
even sequence. It clears the consumed handle only after the clock is even.

There is deliberately no cancel operation. Between claim and publish a future
`jit.attach()` caller may perform only its already-prepared bounded table CAS.
It cannot wait, allocate, use a `lua_State`, safepoint, throw, run a barrier or
invoke a hook. Even an uncommitted post-claim table collision must call publish
once and use that generation as conservative invalidation before aborting its
table guard outside the odd interval.

## Bounded readers

The helpers live in `lj_vmevent.h/.c`, next to the handler lookup they will
eventually protect. This keeps table-writing policy out of the structural
descriptor and avoids adding dependencies to `lj_tab` or `lib_jit`.

`lj_jit_event_attachment_snapshot()`:

1. validates the output pointer, global main TG and lane bound;
2. acquire-loads the authoritative sequence and refuses an odd value;
3. acquire-loads the two generation scalars;
4. rechecks the sequence once; and
5. accepts only an exact canonical shape.

It never spins, waits, dereferences a secondary TG, or treats corrupt storage
as absence. Odd, changed, out-of-range, half-initial and generation-mismatch
states return `RETRY` with a cleared output snapshot. No-JIT builds expose the
same configuration-neutral API but always return `RETRY`, because they have
no authoritative clock storage.

Canonical and initial-idle predicates are file-local parts of the tri-state
snapshot operation. They are intentionally not exported: a caller must not
mistake the cleared output returned with `RETRY` for an authoritative initial
clock by testing its bytes separately. There is likewise no array-wide “all
clocks idle” authority: eight sequential snapshots have no shared
linearization point. Bootstrap tests may iterate lanes while no writer exists,
but future runtime admission and lookup must decide from the one event lane
involved in their transaction.

## Focused evidence

`t-jit-attachment-clock.c`, registered as
`m6_jit_attachment_clock`, covers:

- zero initialization of all eight main and secondary clocks;
- exact signed registry keys for all five known event lanes and unknown-key
  refusal;
- explicit-length embedded-NUL compatibility with the stock hash seed/mixing
  split;
- valid lane endpoints plus one-past-end and `UINT32_MAX` refusal;
- odd sequence and malformed stable-state refusal;
- exact initial and published canonical shapes;
- writer `CLAIMED`, `BUSY`, `EXHAUSTED` and `CORRUPT` classifications;
- mandatory cache invalidation and canonical even completion;
- final sequence/generation publication without wrap;
- deterministic same-lane exclusion and different-lane overlap;
- real same-lane and independent-lane contention through the portable `LJThr`
  substrate;
- proof that only the main-TG copy is read; and
- a real `LUAJIT_DISABLE_JIT` library/fixture build in which mapping remains
  available while snapshot and writer authority remain fail-closed.

## Required next transaction

Production wiring must now compose the prepared exact handler-table mutation
with `writer_claim()` and mandatory `writer_publish()`. All allocating,
rooting, hashing, table growth and potentially throwing work happens before
claim. A committed-plus-STALE table CAS still owns this publication: it must
publish and finish before a higher layer performs any fresh semantic retry.
An uncommitted post-claim collision publishes conservatively before abort.

Handler preparation must then become tri-state: an exact rooted function plus
generation, a stable absence plus generation, or a bounded retry. Only after
that reader and durable pending delivery are implemented may the TRACE stream
consume this clock instead of its provisional caller-supplied nonce. Direct
debug-registry mutation remains deliberately unclocked and racy; attachment
generation therefore never substitutes for rooting or exact function
identity.
