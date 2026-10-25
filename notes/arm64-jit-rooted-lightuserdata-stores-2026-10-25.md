# Existing-key lightuserdata stores

The experimental ARM64 rooted-store path now accepts ordinary lightuserdata
keys for scalar and collectable values, and lightuserdata values through the
existing three-root helper. The latter retains its `lj_tab_storegc_rooted`
name to avoid renaming the established interface. Lightuserdata is an opaque
segmented token; it is not dereferenced or treated as a collectable object.

The runtime keeps the existing protocol: reserve nil anchors before acquiring
leases, confirm the original table/key/value cells and owner, publish exact
inputs into distinct TG anchors, and release temporary authority before the
cooperative existing-key store. The current-generation keyed CAS, forwarding,
barriers, errno preservation and error rollback remain unchanged. The recorder
selects IN1, IN2 and IN3 for the three-root form. Semantic and allocated
admission independently require matching source types, root modes, CARG order,
status guards, register classes and snapshots. There are no emitter changes.

This covers ordinary LOOP and FORL roots. It does not open new-key insertion,
nil deletion, general metamethod traces, heap-effect side traces or raw stores.
A deleted key causes the status guard to exit and ordinary Lua assignment
handles insertion. Nil and NaN assignment keys remain rejected.

## Internal-marker boundary

Table FORWARD, KEYLOCK and persistent resize markers share the lightuserdata
tag but are not ordinary values. A direct probe of the first isolated draft
showed that the widened helper returned success for `LJ_TFORWARD_BITS` as a
value. Log: `/tmp/ljmt-lightud-internal-baseline.log`; source:
`/tmp/ljmt-lightud-internal-probe.c`. This draft was never committed.

The final implementation excludes table-internal keys before root handoff and
uses the existing keyed-store value validator for lightuserdata values.
Direct fixtures require each marker to be refused, input bits and table
contents to remain unchanged, root/SMR state to be clean, and no retry wait.

## Validation

On frozen runtime sources based on `9a7b8716`, the final serial run passed:

- The complete rooted-GC contract on ARM64 and arm64e/BTI, assertion and
  release, including the earlier nil/scalar/GC/lightuserdata reads, segment-map
  races and lifetime checks.
- Native LOOP stores with lightuserdata keys and NUM/boolean/GC/lightuserdata
  values, ordinary string/GC keys with lightuserdata values, and FORL variants.
  Warm calls require one root, no aborts and 19--21 native exits over twenty
  calls; all observed twenty. Post-GC token identity remains exact.
- Deleted-key fallback identifies the helper status guard's snapshot and
  requires exactly one exit through it before interpreter insertion, with the
  same loop bound used for warm native execution.
- Authentic LOOP/FORL clones independently reject wrong helpers, root aliases,
  nil/raw-pointer producers, incompatible registers and CARG sources.
- Direct rooted stores cover scalar/boolean lightuserdata keys, three-root
  handoff, source-cell removal, stack relocation, collection, resize, rollback,
  invalid/internal keys and values, reservation failure and clean authority.
- ARM64 and signed x86-64/Rosetta stock source and original-v2 import: 509
  cases in each corpus on each target. The signed x64 run also passed the
  rooted-store, nil-reader, bounded-reader and raw nil-deletion fixtures.

Driver: `/tmp/ljmt-lightud-store-regression-final.sh`. Logs:
`/tmp/ljmt-lightud-store-regression-final.log`,
`/tmp/ljmt-lightud-store-rooted-gc-contract-final.log`,
`/tmp/ljmt-lightud-store-arm64-stock-{source,import}-final.log` and
`/tmp/ljmt-lightud-store-x64-regression-final.log`. The ARM64 experimental
assertion development profile was restored after the run. These are focused
checks, not a new combined bootstrap/experimental or sanitizer result.
