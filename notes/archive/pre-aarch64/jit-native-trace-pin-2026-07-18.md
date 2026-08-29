# Exact native trace pin substrate (2026-07-18)

## Status and scope

This change lands the first implementation prerequisite from
`generic-traced-ffi-calls-2026-07-10.md`: a lock-free lease which keeps
one exact finalized `GCtrace` body, its trace-number reservation, and its mcode
references alive across a future generic traced foreign call.

The substrate is deliberately dormant. The unconditional `LJ_TRERR_BLACKL`
gate in generic `crec_call()` remains, remote safepoint acknowledgement still
uses the existing conservative JIT/native rules, and no broad JIT-active or
flush veto was removed. Consequently this commit cannot make `IR_CALLXS`
reachable for an unsupported traced FFI call.

This is a documented refinement of the activation sequence, not a change to
the files in `plan/`. It replaces any prospective trace-number-only lifetime
scheme with an exact body lease, because a retired trace number can eventually
be reused while a post-call exit still needs the original snapshots and mcode.

## One-word admission and counting

`GCtrace.native_pins` is one 32-bit atomic control word:

- bit 31 is `TRACE_NATIVE_PIN_CLOSED`;
- bits 0..30 are the exact native lease count.

The field occupies four bytes of the existing x86_64 alignment gap before
`retire_epoch`. The target compiler reports `native_pins` at offset 136,
`retire_epoch` at 144, `retired_next` at 152, and `sizeof(GCtrace) == 160`;
the latter three values are unchanged by the insertion.

Pin acquisition requires an independent exact-body lifetime proof. The current
tests hand off from an SMR reader; activation will hand off while `jit_base` and
the current JIT-execution protocol still protect the body. A raw, unleased
pointer is not valid input.

Retirement keeps its existing encoded-epoch linearization point, then closes
pin admission before it decides what to do with the public slot. Both pin and
close use CAS on the same word, so the race has only two outcomes:

1. pin increments first; close retains that count and slot retirement must keep
   the exact public reservation;
2. close wins first; the pin attempt observes `CLOSED` and fails.

There is no two-word count/epoch observation gap. Nested native users increment
the low count independently. Unpin preserves `CLOSED`, rejects underflow, and
the final release of a closed body publishes a notification sequence.

## Reclaim and teardown

A mature retired body with a nonzero count stays on `J->retiredtraces` before
debugger teardown, slot release, root unlink, or physical destruction. Keeping
the body on that list also makes the existing mcode-reference scan retain every
executable area named by the exact body. Immediate, runtime, terminal, and slot
release paths all reject a nonzero count. VM-close preflight checks active slots
and the retired list before freeing the trace vector or compiler buffers; a
live native lease at universe teardown is a fail-stop lifetime violation, not
garbage to clear forcibly.

Long foreign calls must not turn bounded reclaim into a hot detach/requeue
loop. `J->trace_pin_release_seq` changes only on the final unpin of a closed
body. Trace and mcode reclaim memoize both the completed epoch and that sequence.
A pinned-only mature list is skipped until either value changes, while ordinary
inbound-link, debugger, root-unlink, or validation failures retain their
same-epoch retry behavior. The mcode reference query distinguishes an ordinary
active reference from a pinned-only reference for this purpose. No unpin path
writes token-owned reclaim memo fields.

## Deterministic evidence

`tests/t-jit-trace-retire.c` now proves:

- SMR-to-pin lifetime handoff;
- nested count 0 -> 1 -> 2 and staged releases;
- a real compiled trace is retained through a full flush;
- pin attempts fail after retirement closes admission;
- the exact public slot remains reserved through mature reclaim;
- a stable pinned-only list is memoized rather than repeatedly detached;
- final release invalidates that memo and permits bounded same-epoch cleanup.

`tests/t-jit-mcode-retire.c` pins a real trace whose mcode lies in the exact
`MCodeRetire.area .. area+size` interval, performs the production no-throw full
flush, and proves mature trace and mcode reclaim both retain that area until the
final unpin. The same release notification then permits trace and mcode cleanup
without waiting for another grace epoch.

The focused `m5_jit_trace_publish` gate builds both static and dynamic runtime
objects with `-Werror`, runs both fixtures, and retains its existing trace-slot
smoke tests.

## Remaining activation work

The next safe slice is still dormant per-TG native-frame publication. It must:

- consume `IR_XSAVE` staging into stack-relative offsets;
- pin the exact `lj_ir_ktrace()` body before publishing an even stable frame;
- publish `in_native` last and keep the frame stable through consumed-poll
  safepoint acknowledgement;
- scan stack storage only under a native-park/consumed-poll certificate until a
  separate stack-storage lease exists;
- retain the pin through callbacks, unwind, forced post-call exit, and exact
  snapshot restoration;
- preserve all current conservative scanners and JIT-active/flush vetoes until
  those paths have deterministic coverage.

Only after that frame, GC root scan, callback/unwind, and post-call-exit protocol
is complete may the generic `crec_call()` blacklist be removed.
