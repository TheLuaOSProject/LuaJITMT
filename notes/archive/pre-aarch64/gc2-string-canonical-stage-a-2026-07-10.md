# GC2 canonical string lifecycle: Stage A (2026-07-10)

## Landed boundary

This checkpoint implements Stage A of
`gc2-string-canonical-quarantine-protocol-2026-07-10.md` while keeping
`LJ_GC2_STRING_BODY_RECLAIM == 0` hard-disabled.

Every `GCstr` now has an aligned atomic canonical-lifecycle word. New strings
and the embedded empty string begin in `LIVE`. On x86_64 the private layout is:

```text
sid=12, hash=16, len=20, canon=24, payload=32
```

The existing hot header offsets are unchanged. DynASM, the JIT string-reference
IR, the FFI recorder and allocation/free sizing derive the payload offset from
`sizeof(GCstr)`, so a clean rebuild regenerates their offsets. Assertions pin
the x86_64 layout and the adjacency of `global_State.stremptyz` to the embedded
empty-string header. Public Lua/LuaJIT API, ABI and bytecode formats are
unchanged; private-header consumers must rebuild.

`global_State.str` also owns a secondary canonical quarantine hash. A normal
main-table hit does not touch it. A true main-table miss checks quarantine
before allocation and checks it again before the main bucket publication CAS.
An acquirable `QACTIVE` or `QCLOSING` record is atomically changed to
`QRESCUED`; `QRESCUED` returns the same body. `QCOMMIT` and `FREEING` are
non-acquirable and remain disabled until the read-epoch/publication stages land.

Quarantine headers have per-TG pins and an opportunistic exact topology claim.
Header growth aborts instead of waiting when a reader is pinned, re-chains only
after readers are excluded, release-publishes the replacement, and retires the
old header behind the existing epoch/SMR drain. Bucket-visible records are
retained until terminal shutdown in this stage. GC2 marks the record metadata
and conservatively preserves authoritative quarantine bodies; this is an
intentional Stage A/B retention rule, not authorization for logical death.

The `qcount` fast-path is exact while representable and saturates permanently at
`UINT32_MAX`. Its zero value is a correctness assertion that quarantine is
empty, not merely a sizing estimate: modular wrap to zero would let a true main
miss bypass quarantine and publish a second identity. Stage A never removes an
authoritative record. The later Q-unlink implementation must maintain a
separate exact live-record count rather than decrementing a saturated value.

The production tag/unlink prototype is still unreachable. The focused test
helper can publish a record, arm `QACTIVE`, and detach one main edge solely to
exercise the real lookup and shutdown paths.

## Ordering proof exercised

The standalone C11 model exhaustively explores one collector and two interners
under the required ordering:

```text
publish Q record -> arm QACTIVE -> unlink main edge
main miss -> consult Q -> only then publish fresh body
```

The valid protocol completed 428 bounded schedules with no absence gap or
duplicate identity. Its self-check found 319,608 gap schedules and 2,170
duplicate schedules when unlink was deliberately moved before quarantine, and
50 duplicate schedules when the main miss deliberately skipped quarantine.
It also covers an eight-way `QACTIVE -> QRESCUED` race and 12,000 rounds of
acquire/release stress.

The linked fixture verifies empty/new body layout and NUL termination, duplicate
intern identity, survival through a full GC2 cycle, and exact terminal string
accounting. Two deterministic pause-hook races exercise the implementation
rather than only the abstract model:

1. starting with `qcount == 0`, a detacher stops after publishing non-zero
   `qcount`, unlinking the exact main edge, and releasing the topology claim;
   another interner observes the real main miss and must return the same body
   through Q, changing `QACTIVE` to `QRESCUED`; and
2. a second record publisher stops after acquiring the current Q-header pin but
   before its bucket CAS (the fixture verifies that the record is still absent
   from the bucket and the body is still `LIVE`) while another OS thread
   attempts a header resize; the resize must abort, after which publication
   completes and a later resize re-chains the record.

The explicit test barrier makes the first case a program-state/order regression,
not a claim to be a hardware memory-order litmus. It proves that the production
ordering exposes no main-miss interval with the Q fast path disabled.

The fixture also forces `qcount` through `UINT32_MAX-1`, proves saturation at
`UINT32_MAX`, and proves another increment cannot wrap it to zero.

## Deliberate temporary costs

The proof-first lifecycle word adds eight requested bytes per string. With the
current 16-byte arena cells, strings with one through seven payload bytes move
from 32 to 48 physical bytes. Other length bands alternate between no physical
increase and one extra cell. The 32-byte payload alignment may improve some
loads, but short-string resident memory and intern throughput must be measured.

The final performance shape may move `canon` to a lazy arena side plane and
pool quarantine records. That optimization is allowed only after the state
machine and address-validation tests pass; it may not reintroduce a mark-only
unlink decision or require an ordinary string read to chase forwarding.

An interleaved nine-run release benchmark, pinned to one x86_64 CPU with GC
stopped, measured the immediate cost against commit `9ed68417`:

| Operation | Before | Stage A | Delta |
| --- | ---: | ---: | ---: |
| Existing four-byte intern hit | 62.441 ns | 62.464 ns | +0.04% |
| New unique four-byte string | 1,394.7 ns | 2,010.3 ns | +44.14% |
| Warmed JIT byte/length loop | 4.873 ns | 4.911 ns | +0.78% |

For 300,000 four-byte strings, requested memory grew by exactly 2.4 MB and
occupied arena space by exactly 4.8 MB; mapped arena growth was 4.75 MiB and
median RSS growth was 4.91 MiB. Existing intern hits and steady JIT byte access
are effectively flat, but the short-string creation/residency result is not an
acceptable final performance shape. Moving lifecycle state to a lazy compact
side plane is therefore a required optimization before the release gate, not
merely an optional cleanup.

## Work still required before reclamation

Stage A deliberately does not claim that a generic late publication is safe.
The following remain mandatory:

- `LIVE -> CANDIDATE` before `DEAD`, followed by publish-before-unlink
  quarantine ownership and same-body re-link;
- centralized rescue in every strong root/marker path;
- prepare-before-store for stack, table, upvalue, channel, VM, API and JIT
  publication;
- per-TG pre-load read epochs, whole-trace coverage and precise native string
  byte borrows;
- `QCLOSING -> QCOMMIT` arbitration, directory unlink, the second reuse grace,
  pooled metadata, table shrink and address-reuse stress; and
- Linux sanitizer/race stress plus Wine and Darling target validation.

Until those stages pass, quarantine bodies are retained and production string
body unlink/free remains disabled.
