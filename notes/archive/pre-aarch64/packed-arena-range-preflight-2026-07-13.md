# Packed arena range preflight

## Motivation

The pushed typed-sweep amortization checkpoint `54714887` reduced repeated
quarantine visits and global certificates, but a fresh loop-only hardware
profile still measured 405.96 ns per `closures_upval` operation. The remaining
free-run reconstruction path was the clearest isolated target:

```text
arena_clear_extent_range   51.42 ns/op
arena_set_free_run         32.02 ns/op
lj_arena_scan_free_runs    13.95 ns/op
total                      97.39 ns/op
```

The first two functions repeatedly loaded root, recovery, destructor, READY,
and lifetime state for every cell in a large free span. The same metadata is
already stored in packed atomic planes, so this was observation overhead rather
than useful semantic work.

## Exact packed proof

`arena_range_ownership_preflight()` now walks only the bitmap words intersected
by the requested range. Existing exact converters map every nonzero root,
recovery, and destructor state and every non-FREE lifetime nibble back to one
bit per arena cell. Each word is intersected with a partial-range mask, so cells
before or after the requested span cannot veto it.

This is only an amortized form of the previous acquire observations. It does
not authorize mutation, replace the owner/open-or-closed generation protocol,
or remove any later validation. `arena_set_free_run()` still completes its
entire first preflight before changing metadata, scrubs READY/cdata/destructor
coverage, and calls the stricter `arena_clear_extent_range()` preflight again
before removing interior boundaries.

The two historical policies remain distinct:

- `arena_set_free_run()` treats recovery ownership as an ordinary fail-closed
  veto and returns without mutation;
- `arena_clear_extent_range()` retains its corruption assertion and abort when
  recovery is the first cell-ordered blocker.

The cold fatal path also preserves the old diagnostic order inside a packed
word. An ordinary blocker in a lower cell returns first; recovery at the same
or an earlier cell remains fatal. Zero-length free-run publication is now
rejected explicitly before end arithmetic or structural mutation. Other
invalid bounds are rejected before an array access.

READY deliberately differs between the two callers. An old typed READY bit
vetoes direct extent removal, while free-run publication is allowed to scrub
READY after lifetime FREE and zero destructor identity. Its second interior
preflight then observes the scrubbed plane exactly as before.

## Race and ordering argument

The packed loads use the same acquire order class as the former per-cell
accessors. Release publication of `block[]` changes and the atomic relaxed AND
on concurrently markable `mark[]` are unchanged. A racing allocation,
recovery, root, or remote-free publication is still governed by its lifetime
claim and the surrounding arena generation. Grouping reads by word does not
make a stale snapshot commit authority; later validation and the exact
open/commit generation continue to reject a conflicting publisher.

For a valid nonzero range, the new predicate is logically identical to the old
one:

- every root/recovery pair and lifetime nibble is converted exactly;
- every destructor plane is ORed into cell geometry;
- READY is included only for the caller which historically required it; and
- plain arenas continue to omit the traversable-only lifetime plane.

An independent concurrency/source audit found no unsafe-positive race or
memory-order weakening after the recovery-policy, zero-length, and cold-order
hardening.

## Deterministic coverage

An `LJ_ARENA_TEST_HELPERS` wrapper calls the actual static free-run function;
it is absent from production builds and changes no public ABI. The focused
arena-sweep fixture uses one 20-cell range spanning both packed lifetime
positions 15/16 and bitmap positions 31/32. Whole-arena snapshots prove that:

- zero-length and invalid-bound calls fail without mutation;
- root, recovery-PENDING, destructor, and non-FREE lifetime blockers each fail
  without changing any arena byte;
- recovery remains a nonfatal veto on the set-free path;
- blockers immediately outside the partial range do not leak through its
  masks; and
- a valid run scrubs READY/cdata, removes all interior block/mark boundaries,
  and publishes exactly one mark-only free-run start.

The fixture compiles with `-Wall -Wextra -Werror` and passed.

## Validation

All requested forced-clean gates passed:

- `m2_arena_sweep`, before and after the direct packed-boundary fixture;
- `m2_arena_gcsweep`;
- `m3_gc2_recovery` in normal and assertions/GC2-paranoia builds; and
- `m3_gc2_paranoia`, including all C oracles, 509/509 JIT tests, and 387/387
  no-JIT tests.

Every configuration-changing run restored the default build. The only known
compiler diagnostic was the pre-existing GCC inlining warning around
`gc2_root_rescan_later`/`la_load32_acq`.

## Performance

Five fresh independent processes with the valid stopped-GC wrapper measured:

```text
current active:  319.30, 317.84, 318.53, 320.70, 323.47; median 319.30 ns/op
current stopped:  94.83,  94.02,  95.16,  94.28,  95.99; median  94.83 ns/op
stock active:     37.51,  38.45,  36.98,  39.65,  38.28; median  38.28 ns/op
stock stopped:    18.35,  21.46,  21.04,  20.61,  21.70; median  21.04 ns/op
```

The final zero-length/diagnostic hardening was performance-neutral in a
three-process confirmation: 319.44 ns/op active and 94.14 ns/op stopped.

Against pushed `54714887` (389.19 active, 113.31 stopped), the packed preflight
improves active time by 17.96%, stopped time by 16.31%, and active-minus-stopped
cost by 18.63%. It is 34.68% faster than the earlier 488.80 ns/op typed-body
checkpoint. The main five-process medians remain 8.34x stock active and 4.51x
stock stopped, so b1.2.0 is still performance-blocked.

The next profile-directed tranche is an independently revalidated terminal
FREE/FREEING word fast path in quarantine bitmap readiness/application,
followed by all-free live-cell counting/adoption if the new profile still
justifies it.
