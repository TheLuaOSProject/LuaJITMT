# JIT VM-event attachment-clock substrate (2026-07-19)

## Scope and current limitation

This tranche adds only the dormant publication-clock storage and bounded
reader contract required before `jit.attach()` can be coupled atomically to
the handler table. It does not change `jit.attach()`, handler replacement,
handler lookup, VM-event callback ownership, TRACE delivery, or callback
behavior. Every clock therefore remains zero in production today.

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
A future writer that completes an odd interval must publish a nonzero
generation, including a conservative invalidation after it has claimed the
clock but collides during the exact handler-table store. A failure before any
semantic mutation may instead restore the exact original even sequence.
Consequently, advancing the stable sequence without a generation would mean
that the table/clock transaction was only partly published and readers must
refuse it rather than silently call a mismatched handler.

Generation and sequence zero are never reused after a completed publication.
The final nonzero generation (`UINT64_MAX`) and final even sequence
(`UINT64_MAX-1`) remain readable canonical states. The future writer must
refuse before either counter would wrap; this structural tranche intentionally
does not add that writer yet.

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
- valid lane endpoints plus one-past-end and `UINT32_MAX` refusal;
- odd sequence and malformed stable-state refusal;
- exact initial and published canonical shapes;
- final sequence/generation observability without wrap;
- proof that only the main-TG copy is read; and
- a real `LUAJIT_DISABLE_JIT` library/fixture build in which the API remains
  fail-closed.

## Required next transaction

Production wiring still needs one nonthrowing odd interval which composes the
already-prepared handler-table mutation with generation publication. All
allocating, rooting, hashing, table growth and potentially throwing work must
happen before that interval. Handler preparation must then become tri-state:
an exact function plus generation, a stable absence plus generation, or a
bounded retry. Only after that transaction and durable pending delivery are
implemented may the TRACE stream consume this clock instead of its provisional
caller-supplied nonce.
