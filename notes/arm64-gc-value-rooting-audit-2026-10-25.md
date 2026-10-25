# Remaining GC-value lifetime boundary in ARM64 rooted table effects

This records the design audit at `effe6467`. The subsequent implementation and
validation are recorded in
[`arm64-jit-rooted-gc-values-2026-10-25.md`](arm64-jit-rooted-gc-values-2026-10-25.md).
The initial lifetime alternatives below were superseded by the GC2 native
phase/reclamation gate proof in that note.

## Stores

The forwarding-aware `lj_tab_storetv_existing_forjit` already re-resolves the
key, owns cooperative retries and applies `tab_store_barrier_write` before
retrying and after publishing. Its barriers cover parent/key/source
publication, weak-table writes and GC2 table-value publication. Replacing that
store protocol or admitting raw HSTORE/ASTORE is not needed for this extension.

The missing wrapper lifetime is explicit in `tab_storescalar_rooted`: it
leases, confirms and anchors the table and key, but its value is numeric or
boolean and has no GC edge. A GC-valued source needs its own exact input
TValue, lease and enumerated anchor. All inputs must survive any anchor
reservation, stack growth, retry, STOPREQ or GC phase change; no lease or SMR
scope may cross the cooperative retry boundary. The store must consume the
anchored source, with the existing protected-frame cleanup restoring anchors
on nonlocal exits.

Generated `IR_TMPREF` currently supplies only `tmptv` and `tmptv2`. A general
table/key/value operation therefore needs a third certified input root or
another explicit lifetime design. Reusing either existing slot would clobber
an input. A raw pointer kept only in a C parameter is not an enumerated root.

## Reads

`lj_tab_gettv_rooted_try` already accepts GC-valued results. It leases and
validates the result and publishes the complete TValue to the output root
before dropping the lifetime scope. The table can occupy IN1|OUT1 and the key
IN2, so this direction does not need a third scratch slot.

The recorder currently selects the scalar try helper only for numeric and
boolean results; other values use the waiting helper. Native admission would
need a precise operation contract for the general try helper, its success
guard, typed VLOAD and post-helper snapshot in both independent validators.

The result lifetime after VLOAD is a separate constraint: a register or spill
can retain the GC pointer after another helper overwrites the output root.
The helper's valid return alone does not certify that later native lifetime.
The initial audit proposed either restricting the last use to precede root
clobber/safepoints or adding persistent native-frame rooting. Further source
inspection established a third mechanism already present in GC2: result
publication marks/rescues the current-cycle object, and active JIT state
prevents phase completion and physical reclamation until snapshot restoration.
This permits GC values across ordinary admitted table helpers without adding
a per-value root buffer. Opaque cdata values do not authorize payload access
or mixed CALLXS graphs.

## Implementation and evidence boundaries

The affected source is concentrated in `src/lj_tab.c` (the rooted read and
store wrappers), `src/lj_record.c` (rooted lookup/store recording),
`src/lj_asm_arm64_admit.h` (separate semantic and allocated-effect checks),
`src/lj_asm_arm64.h` (TMPREF storage), and `src/lj_gc2.c` (TG root enumeration).

New evidence must prove native execution and object identity under GC,
active-peer mutation, forwarding, missing-key/metamethod fallback and error
cleanup. Both validators must independently reject wrong helper IDs, types,
CARG/root order, root aliasing/clobber, missing status guards, incorrect
post-effect snapshots and invalid spill ownership. Runtime helper fixtures
should force handoff while the value survives only through the intended
enumerated root. Existing scalar tests are useful infrastructure, not proof
that GC-valued inputs or results are safe.
