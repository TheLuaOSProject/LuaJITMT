# Owned bounded-drain caller audit

This is a complete direct-caller audit of `gc2_mark_drain_owned_bounded` in the 225 baseline inputs and ROOT's exact v2 patch. The name `_owned` was not sufficient proof for the test entry. ROOT's observed assertion is preserved, and the v2 source verdict remains provisional until that entry receives a real claim.

Baseline source lines refer to `base/src/lj_gc2.c`; v2 lines refer to `review-inputs/candidate/src/lj_gc2.c`.

| Entry to bounded drain | Claim provenance | Release/alternate path |
| --- | --- | --- |
| SWEEP prepare at baseline5667 | `gc2_sweep_prepare_bridge_result` wins `gc2_worker_claim` at5619 and rechecks can_progress. | The claim remains through snapshots, drain, EOF/READY checks and owned result; release at5740. No release precedes the drain. |
| WEAK close round at12602,12627,12653,12672 | Only production caller is `gc2_weak_complete_result`; it wins count-busy claim at12728, rechecks WEAK/native state, then calls the close round. Close round asserts worker_active. | Every early round return goes back to the result's single `out` path; release at12802. |
| WEAK frontier validation at12701 | Same result claim. The frontier helper itself asserts worker_active. | Return does not release; result does so at its `out` path. |
| Weak bridge trace at12203 via backfill | `gc2_weak_backfill_bridge` has one caller: the already claimed `gc2_weak_complete_result` at12780. | Exact table scopes survive trace; original caller retains the claim until `out`. |
| Weak bridge trace at12203 via overflow | Production `gc2_weak_overflow_clear_bridge` caller at12770 is the same already claimed result. | Same held-claim return. There is also an unclaimed test entry described below. |
| MARK fixpoint via22673 with worker_owned=1 | `gc2_mark_close_help` wins `gc2_worker_claim_mark_close` at22902; its only call of `lj_gc2_fixpoint_run` passes1 at22914. That path calls `gc2_fixpoint_round`, then the owned drain. | The helper holds worker_active through fixpoint and MARK→WEAK publication, releasing at22942. |
| Public `lj_gc2_fixpoint_round` | It passes worker_owned=0 at22848. | `gc2_fixpoint_drain` selects the worker-budget/logical drain, which acquires its own claim. It does not directly use the owned drain on an unclaimed public call. |

The static functions have no address-taken/indirect entries in the inspected source. No unclaimed production weak-bridge path was found. The new direct worker and assist selector callers remain inside their original worker claims. Their active-TG choice is unchanged: worker passes NULL, assist passes its supplied TG, closure passes its original `G2TG`. `lj_thr_get_tg_fallback` at `lj_thr.c:1687` returns a matching TLS TG or main TG only when its actor matches the current physical actor. This review does not broaden any owner-private authority.

## Confirmed unclaimed test path

At baseline12412 (v2 12402), `lj_gc2_test_weak_overflow_clear_bridge` simply calls the static overflow bridge. That function can call bridge_trace_tab, which calls the owned bounded drain with UINT_MAX. The test wrapper acquires no claim.

All three fixture calls are in the unchanged `tests/t-gc2-traverse.c`:2874,2880,6228. `test_weak_overflow_retry_stops_bridge` first injects a one-shot value-admission refusal and expects the bridge to retain both entries. Its second call at2880 expects successful clearing and reaches the graph-drain path. The earlier refusal returns before that path. The headless reservation test at6228 also returns before graph service. None of those calls establishes worker ownership.

ROOT's independently executed strict GDB observation in `review-inputs/diagnosis-v2/traverse-gdb.json` shows precisely:

```
test_weak_overflow_retry_stops_bridge:2880
  -> lj_gc2_test_weak_overflow_clear_bridge
  -> gc2_weak_overflow_clear_bridge
  -> gc2_weak_bridge_trace_tab
  -> gc2_mark_drain_owned_bounded(limit=4294967295)
  -> gc2_graph_drain_one_owned:22103
  -> assertion: graph selection without worker
```

The GDB artifact SHA256 is ddcb6acc9095fc8536a44949b7a0fbd17be4f9f32369d4d10b642c4fc1e8ac7b. It is ROOT's runtime/debugger evidence; this reviewer ran neither fixture nor debugger. The old helper already performed owner-side graph operations without a token on this path. The new assertion exposes that missing test-entry contract; it does not justify asserting an analogous production failure.

## Smallest source repair proposal

Change only the test wrapper to claim on behalf of its test caller, call the existing static bridge, then release. Null input or failed claim returns zero. For example:

```c
int result;
if (!g || !gc2_worker_claim_count_busy(g))
  return 0;
result = gc2_weak_overflow_clear_bridge(g, bridge_head);
gc2_worker_release(g);
return result;
```

This is a proposal, not an applied patch. The static bridge retains its WEAK checks and exact scopes; the wrapper owns the claim across all internal traversal and drain work and releases it for both positive and negative results. A failed claim must leave the queues, weak slots and scheduling hint untouched apart from normal busy telemetry. Existing callers do not pass an already-owned token. A future owned caller should call the static owned implementation, not infer ownership from `worker_active != 0` or bypass a failed claim.

Do not delete the selector assertion, set worker_active directly, or let the wrapper treat another actor's active claim as its own. The wrapper remains a controlled test bridge, not a full production weak-completion API: it intentionally runs beneath the production closure certificates, and does not replace the production native/root/fixpoint gates. Its current tests control native state. Validate the unchanged retry-then-success and headless tests, plus a held-claim refusal and release-after-refusal control. Preserve the original v2 abort separately.
