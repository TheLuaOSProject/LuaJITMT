# Bounded current-FORWARD repair

Date: 2026-07-25

This Linux/x86-64 checkpoint restores progress when a table operation's
bounded resolver attempt finds a `FORWARD` marker in an otherwise stable
current generation. It fixes a pre-existing regression in the setter cutover
to bounded resolver attempts without weakening the classification of a real
in-flight resize and without adding a lock or work to the ordinary missing-key
insertion path.

This is not the persistent resize descriptor. A real resize owner can still
hold the only copy of a moving value in a C local, so a peer which observes
that live hand-off must continue to retry. The descriptor remains the next
structural tranche.

## Regression evidence

`m5_tab_forward_filter` constructs a current separated array whose exact slot
contains `FORWARD`, with no successor publisher or structural owner, and then
calls `lj_tab_setint()`. The setter introduced by `f37baada` classified every
`FORWARD` as generic `RETRY`, so this stable state waited forever.

The timeout reproduced at exact pre-monotonic-newkey baseline `c832d41d`; the
parent of `f37baada` passed the same fixture. This establishes the bounded
resolver cutover as the regression boundary and keeps the repair independent
of the monotonic shared-new-key checkpoint.

## Internal classification

The keyed resolver now has two internal-only terminal results in addition to
`FOUND`, `ABSENT`, and `RETRY`:

- stable current separated-array `FORWARD`; and
- stable current hash `FORWARD`.

The classification is valid only after the same paired array/hash generation
proof used for ordinary keyed resolution. It also relies on these publication
facts:

- a shared resize owns `struct_owner` before it can publish `FORWARD` into a
  current vector;
- hash resize publishes `RETIRING` before replacing a value with `FORWARD`;
  and
- a private colocated resize may freeze its inline current slots without
  `struct_owner`, so current colocated `FORWARD` remains `RETRY`.

After acquiring a candidate marker, the resolver acquires `struct_owner`
before its final paired-root validation. A nonzero owner means a live hand-off
and returns `RETRY`. A zero owner plus a still-current, nonretiring paired
generation proves a stable orphan marker. A resize which starts after the
owner check cannot be the publisher of the marker already acquired before
that check.

The raw result never exposes the old slot pointer. Only a freshly validated
`FOUND` result may return a slot.

## Caller contracts

Public bounded reads and existing-only stores map either stable raw
`FORWARD` result to logical `ABSENT`:

- rooted reads return nil/absence;
- held/rooted keyed-slot resolution returns `ABSENT` with address zero; and
- existing-only JIT stores return failure and deopt without mutating or
  resizing the table.

Insert-capable callers consume the internal distinction and repair it:

- separated-array `FORWARD` forces a resize which drops the orphan marker,
  then re-enters the integer setter for another bounded resolver attempt;
- hash `FORWARD` rehashes with the exact key anchored in a TG root; and
- ordinary integer, string, and generic setters use the same repair path.

Insert-capable JIT helpers treat the stable marker as absence in their current-
slot lookup and call the ordinary setter, which consumes the raw status and
repairs it. Their keyed CAS remains the final validation before publishing the
store. No pointer from the retired generation is dereferenced after repair.

## Cold retry hardening

Rehash and no-free/chain-overflow paths may allocate and lose another
generation before their raw return pointer can be consumed. Their exact key is
already anchored, so they now discard that pointer and loop through a fresh
bounded paired-generation resolution. Stable `FORWARD` is repaired, a live
handoff waits, and only a fresh `FOUND` pointer escapes.

The two exact-existing matches inside shared `tab_newkey_impl()` receive the
same narrow validation: load the value, prove the node generation is still
current and nonretiring, reject claims or malformed values, and repair a
stable `FORWARD`. This validation is limited to the rare exact-existing branch.
The common missing-key scan, private insertion, and successful new shared
insertion paths remain unchanged.

## Performance boundary

The repair adds no new mutex or unconditional common-path ownership operation.
Its additional generation/value validation executes only after an exact-
existing new-key match, and its repair loops execute only after a stable orphan
marker or a slow rehash path. The cold repair deliberately invokes the existing
resize/rehash structural-owner path and can still allocate or wait; it fixes
the orphan livelock but does not close the zero-wait gate. The stock-like
private path and the ordinary active-MT missing-key path are unchanged.

The repair helper is noinline, so the rare path does not enlarge the ordinary
setter frame. GCC 14.2 release assembly shows the final resolver no longer has
an extra common-path `FOUND` branch, `lj_tab_setint()` retains its baseline
frame size, and the `lj_tab_newkey()` wrapper is byte-identical. The complete
`lj_tab.o` text grows by 416 bytes, including the 157-byte cold repair helper.

Pinned alternating measurements against exact parent `7df564cb`, with a peer
held active so shared-table protocol remained live, found no attributable
regression at the resolution of the run:

- JIT shared unique insert: 1356.85 to 1343.99 ns/op;
- JIT shared existing-key set: 1153.14 to 1136.58 ns/op;
- interpreter shared existing-key set: 1184.25 to 1174.09 ns/op;
- interpreter shared unique insert: 1208.45 to 1212.98 ns/op; and
- prebuilt-key shared first insert: 2029.84 to 1993.03 ns/op.

Run-to-run variation was 2-9% under a loaded container, so sub-percent
differences are noise rather than wins or regressions. Private traced
existing-store, private unique-insert, and private interpreter existing-store
guards likewise showed no directional regression. Collision-heavy lookup and
comparison against an actual stock LuaJIT build remain release evidence debt.

## Coverage

Deterministic coverage checks:

- current colocated array `FORWARD` remains `RETRY`;
- an explicit shared structural owner makes current hash `FORWARD` return
  `RETRY`, while removing that owner exposes stable `ABSENT`;
- stable separated-array and hash markers repair through rooted
  insert-capable resolution and then resolve as fresh `FOUND`;
- existing-only JIT stores fail without changing the old generation;
- insert-capable JIT array/hash stores repair and publish the requested value;
- direct numeric-hash `lj_tab_setinth()` repairs and stores; and
- retiring and mixed generations retain their existing `RETRY` contract.

The focused final-source matrix includes `m5_tab_forward_filter`,
`m5_tab_keyed_slot_resolver`, `m5_tab_rooted_get_try`,
`m5_tab_colocated_resize`, `m5_tab_newkey_monotonic`,
`m5_tab_keylock_lookup`, `m5_tab_finreg_newkey_stale`,
`m5_tab_cas_store`, `m5_tab_resize_copy_helper`, and
`m6_jit_table_store_helper`. The forward-filter fixture passed 100/100
repetitions and the keyed resolver passed 50/50.

Isolated GCC 14.2 and Clang 19.1 helper/assert builds passed with `-Wall
-Werror`, and all three directly changed fixtures passed under each compiler
with `-Wall -Wextra -Werror`. Clang target-only ASan passed each fixture 10/10
plus a four-writer, two-GC-worker, 256-round resize stress. Clang target-only
UBSan, with the repository's documented low-level-emitter exclusions, passed
the same repetitions and stress. ASan leak detection remained disabled under
the repository policy for retained GC2 arenas/registries. The isolated default
build was restored after both sanitizer profiles.

The canonical cached `m5_concurrent_objects` aggregate passed the repaired
forward/filter/resolver block and continued through later table, JIT, VM-event,
string, ctype, registry, no-metamethod, OS and libc cases. It stopped later at
`m5_state_owner` with LuaJIT's `not enough memory` panic. Running that fixture
directly at current source and at exact baseline `c832d41d` produces the same
failure, while the cgroup reports no kernel OOM event. The aggregate is
therefore not claimed as passing, but its remaining stop is baseline-identical
and separate from this checkpoint.

## Residual boundary and next tranche

This repair deliberately does not treat a live resize hand-off as absence.
Once a source has become `FORWARD`, the resize owner may be paused with the
only payload in an owner-local `TValue`. Helping cannot recover that payload
from the present table state.

The next tranche therefore publishes a persistent resize descriptor before
retirement or forwarding. It must retain exact old/successor generations,
durable per-slot moving state and payload, and helper-safe completion
accounting. Multi-stage pause hooks should cover owner loss after descriptor
publication, source capture, forward publication, successor installation and
root hand-off. FINREG remains a separate follow-on transaction because its
`FINCLAIM` also owns order-list and finalizer side effects.

`m9_newkey_barrier_scope` still exceeds its fixed grey-drain threshold at both
current source and exact baseline `c832d41d`; paired runs found no patch delta,
so it is not claimed as a passing gate here. Full macOS and Windows validation
and fixes remain deferred until b1.2.1 is otherwise release-ready.
