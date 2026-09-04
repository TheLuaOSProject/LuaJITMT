# GC recovery fixture setup repair

The full recovery fixture exposed two pre-existing setup assumptions during
the Linux review. Neither required a runtime change.

`test_grey_growth_transaction` installed a synthetic full capacity-one grey
deque, then flushed its parent SSB before arming growth failure. State
construction had already occupied the other SSB node. The flush therefore
recycled that earlier published work through
`gc2_recycle_published_ssb_for_flush()` and grew the deque before the failure
hook was armed. Observed state moved from top/bottom/capacity `0/1/1` to
`0/3/4`; the worker then appended the parent normally, producing `0/4/4`
with no recovery identity. MARK-close intent remained zero throughout.

The fixture now drains startup work immediately after its explicit MARK
start, before installing the synthetic deque. New assertions require both
the tested parent and child to remain unmarked. All original assertions for
unchanged grey contents, exact SSB-to-recovery transfer, counters, and eventual
child marking remain intact.

`test_sticky_failure_without_items_is_bounded` similarly assumed zero recovery
items meant no real work anywhere. A worker could legitimately drain startup
SSBs despite the injected failure. The fixture now completes an ordinary GC,
stops automatic GC, and explicitly checks that all SSB/grey/recovery work is
empty before injecting the locator-free failure. Its no-progress, unchanged
cursor, and absorbing reclamation-veto assertions remain intact.

The original tests failed against both the captured current archive
(`eb77c111` plus the two `lj_meta.c` pre-store barrier elisions) and an otherwise
identical archive with only `lj_gc2.o` restored from `a649f737`. The first
failure also reproduced against the earlier consistent non-assert archive
from before the meta elisions. It is therefore neither a durable-intent
regression nor specific to the strict assertion/helper configuration.

With both setup corrections, the full fixture passed against current and
original-GC strict archives. Runtime and fixture flags matched:

```text
-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS
-DLJ_TG_ROOT_TEST_HELPERS -DLJ_API_ROOT_TEST_HELPERS -Werror
```

Sources, diagnostic snapshots, isolated archives, and results are retained in
`/tmp/lj-gc-recovery-triage-ndoa_rs3/`. `original.a` changes only the GC object;
it is not a claim of a full historical-tree build. No shared runtime source
or build was changed during this triage.
