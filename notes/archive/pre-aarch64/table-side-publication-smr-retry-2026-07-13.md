# Table side-vector publication under an IDLE SMR writer

## Failure attribution

The heavy threaded JIT-flush stress could finish its mutator work and then spin
in explicit collection with typed activation stuck at `NO_RECLAIM`.  Stable
first-pin instrumentation showed that the final SWEEP state was downstream of
an ordinary transient collision in IDLE:

```
phase=IDLE, smr_readers=0, smr_reclaiming=1
```

The first captured stack was:

```
newhpart_publish
  -> tab_pub_node_mem
  -> lj_gc2_markmem
  -> gc2_markmem_registered_scoped_status_impl
  -> gc2_activation_pin_no_reclaim
```

After making fresh-vector publication tactical, a second capture isolated the
retire-arm path:

```
lj_tab_resize
  -> tab_retire_arm
  -> tab_retire_preserve
  -> lj_gc2_markmem_registered
  -> gc2_markmem_registered_scoped_status_impl
  -> gc2_activation_pin_no_reclaim
```

Both failures were a lost nonwaiting registry-SMR admission against the IDLE
opportunistic reclaimer, not malformed activation or missing semantic work.

## Fresh node and array publication

All `tab_pub_node_mem()` and `tab_pub_array_mem()` inputs are freshly allocated
replacement storage.  The construction or resize attempt owns them directly
before publication.  The table owns them after its node/array root is
release-published.  The raw mark closes a GC root-snapshot race, but is not the
body-lifetime authority in either interval.

These two publication helpers now use
`lj_gc2_markmem_registered_publish_try()`.  A successful admission retains the
existing exact mark validation.  A transient admission loss leaves activation
unchanged and requests the active root-scan retry protocol.  The later
mandatory table-root scan observes the release-published generation and marks
it.

## Node and array retire arming

Resize initializes and publishes each retire record on its global retired list
before setting `armed`.  The list and the resize attempt therefore own both the
record and old vector around the arming edge:

- A reclaimer which observes an unarmed detached record can only requeue it.
- The resize stores the current retire epoch before release-publishing
  `armed=1`.
- A reclaimer which observes the armed state cannot satisfy the required grace
  delay in the same reclaim pass.
- The resize retains its direct record/vector pointers until arming completes.

The pre/post-arm raw marks are consequently publication hints, not standalone
lifetime grants.  Node and array retire preservation now use the same tactical
publication marker.  The mandatory retired-list root scan is unchanged and
remains fail-closed because it has no independent permission to omit a listed
root.

## Focused invariant

`t-tab-node-publish` holds the exact IDLE reclaimer gate and exercises both
forms of publication:

1. It creates fresh separated node and array vectors while ordinary SMR
   admission is denied, and requires activation to remain bit-identical.
2. It resizes a table under the same gate, requires node and array retire
   records to become armed without changing activation, and verifies their
   replacement roots were published.
3. It clears the relevant raw marks, releases the writer, runs the mandatory
   root scan, and requires the current vectors plus both retire records and old
   vectors to be marked again.

This distinguishes independent local/list lifetime from the mandatory retry
which repairs a missed active-cycle root snapshot.

## Validation

- `m5_tab_node_publish` passes with the held-writer invariants above.
- `m5_tab_array_publish` and `m5_tab_retire` pass.
- The publication fixture passes with assertions and `LJ_GC2_PARANOIA=1`.
- Forty telemetry-enabled heavy ASAN/assert stress runs completed without a
  raw-marker SMR failure after both table changes.
- A clean, telemetry-free ASAN/assert retry completed 20/20 heavy runs without
  a timeout, assertion, arithmetic mismatch, or sanitizer report.

## Audit boundaries

`gc2_huge_observed_scoped()` was not present in either first-pin stack and is
unchanged.  Its input is an observational object pointer which does not by
itself prove local or list ownership, so the table/JIT publication argument
must not be generalized to that helper.

Separate telemetry later reached `gc2_recovery_fail_closed()` during MARK.
That is an explicit recovery-classification failure rather than registry-SMR
contention and is intentionally left for an independent diagnosis.
