# GC2 table scan-proof authority saturation

Date: 2026-07-19

This b1.2.1 safety tranche hardens two authorities that the live table-token
cutover will rely on. It does not edit `plan/`, activate the dormant exact
table-token pass, or replace the live `table_rescan_pending` authority.

## Problem

The per-table scan stamp packs a 32-bit dirty epoch with the 32-bit GC cycle.
Both counters previously wrapped:

- a table dirty epoch advanced `UINT32_MAX -> 1`; and
- the GC cycle advanced `UINT32_MAX -> 0`, with zero otherwise reserved for an
  unpublished/reset scan proof.

A sufficiently delayed exact observation could therefore become equal to a
later authority. Making the current full-pass implementation live while those
ABAs remained would turn a bounded certificate into possible false reclaim
authority.

## Resolution

Dirty epochs now saturate. The first bump from `UINT32_MAX - 1` reaches the
maximum normally and invalidates the covered scan cycle. A later bump at the
maximum retains that absorbing value, keeps the covered cycle at zero, and
moves the typed activation authority to sticky `NO_RECLAIM`. Mutator writes
continue; reclamation can no longer be certified from an exhausted identity.

The GC cycle increment primitive is likewise saturating. A cycle-start owner
which observes `UINT32_MAX` pins `NO_RECLAIM` and consumes only its exact start
request before publishing either typed or legacy MARK. Later collection
requests are bounded no-ops under that absorbing veto instead of repeatedly
creating an undrainable request or wrapping through zero.

This is deliberately fail-closed rather than a reset protocol. Reclaiming an
exhausted namespace would require a separately proven joined-world identity
replacement, which is neither needed on realistic timescales nor safe to infer
from counter wrap.

## Evidence and remaining boundary

`tests/t-gc2-traverse.c` now exercises both terminal edges in private
universes. It proves the last ordinary dirty increment, the subsequent sticky
pin, the absence of a cycle-zero publication, exact request discharge, and
bounded rejection of later requests. The helper build, strict `-Werror`
release build, complete traversal fixture, `m3_gc2_recovery`, ordinary GC/FFI
smoke, and retired-symbol gate pass.

The cutover audit also confirmed a separate blocker: the current exact table
descriptor is protected from reclamation but not yet genuinely helpable. A
publisher paused with an ACTIVE descriptor has no production helper capable of
locating its persistent side token and finishing that exact ticket. The
descriptor/token representation and reclamation predicates must be generalized
before production activation; this tranche intentionally does not hide that
remaining work.
