# Table pre-store barrier review

Date: 2026-09-04. Source proof review followed by the narrow `lj_meta.c` change
described below. GC implementation and shared build artifacts are untouched by
this review agent.

The implemented optimization omits the two `lj_gc_pubtab(L, t)` calls in
`meta_tset_rooted_mode()` **only when `kept_roots != NULL`**. That is the complete
`meta_tsettv_pair_mode()` operation used by the VM and current public set APIs,
which owns both slot resolution and the following keyed store. Keep those
barriers for the standalone raw-slot-returning `lj_meta_tset()` / owner ABI.

The required invariants are already explicit in this path:

- The actual receiver and key are copied into enumerated TG anchors under
  exact leases, and root-published before the leases close
  (`meta_chain_capture_inputs()`, `src/lj_meta.c:571`).
- A table-valued `__newindex` carrier is root-published when it replaces the
  receiver, and the chosen target stays rooted through keyed CAS and
  every post-store barrier in `meta_tsettv_pair_mode()`. Removing a pre-store
  parent rescan must not remove either ownership edge.
- Any store that can introduce a semantic GC edge uses the guarded path:
  `tab_store_guard_needed()` is true for a collectable value, or a collectable
  key with a non-nil value (`src/lj_tab.c:6591`). The guard performs post-CAS
  dirty invalidation, weak handoff, key/value preservation, and durable parent
  rescan before retiring its descriptor (`gc2_table_store_handoff()`,
  `src/lj_gc2.c:16200`). A committed store whose generation becomes stale
  still performs that handoff before its caller retries.
- A primitive key/value store introduces no child edge. A newly installed hash
  key whose value remains nil is a tombstone: table traversal skips the key
  until a non-nil value is published. That later GC-key publication takes the
  guard. Key insertion's own publication/weak barriers remain intact.
- Retain the weak-key, weak-value, and value-pair barriers after successful CAS
  in `meta_tsettv_pair_mode()`, as well as all key/root publication, resize
  storage preservation, and failure cleanup. Metamethod calls still leave the helper
  through their existing rooted call frame.

This reasoning does not depend on a sampled `mt_active == 0` or worker count;
it applies to pre-MT and shared operation paths. A single-thread fast-path
predicate by itself is insufficient: GC workers, first-MT entry, phase changes,
and reentrant allocation/callback boundaries must still be accounted for.

This removes redundant pre-store parent rescans. It does **not** establish
insertion's asymptotic cost or resolve the reported performance gap: root
publication of the parent and the guarded post-store parent handoff still
exist. Neither the earlier coarse profile nor this source change proves
quadratic behavior. The root agent's controlled before/after measurement did
not find a meaningful improvement; its measurement record is separate from
this source proof.

Do not generalize this change to the public SWEEP root/barrier implementation.
`gc2_trace_sweep_edge()` intentionally limits its current-table skip to worker
graph discovery (`src/lj_gc2.c:21732`). A public barrier can follow a raw payload
mutation that did not bump the table dirty epoch (`:21739`). Removing the
worker restriction would allow a stale current stamp to hide a newly published
child. `test_current_cyclic_table_private_edge_is_consumed()` explicitly checks
that a public root changes CLAIMED recovery to REDIRTY despite a current stamp
(`tests/t-gc2-recovery.c:1813`). A header NEEDSCAN bit plus current stamp also
does not replace exact rescan-token ownership; the stale-hint case at
`tests/t-gc2-traverse.c:4987` covers that distinction.

Removing post-store dirty/rescan from `gc2_table_store_handoff()` is a separate
protocol change. A scalar *value* can still publish a collectable key, and the
guard's parent lease is body retention, not proof that its new child graph was
scanned. Avoid that larger elision without an explicit edge-complete handoff
and scan-stamp argument.

Existing meaningful verification for the local pre-store change covers:

- Existing `t-meta-rooted-chain`, `t-gc2-public-store-weak-window`, and
  `t-gc2-table-store-guard` fixtures, including the SWEEP finish ordering case.
- GC traversal's actual-target `__newindex` parent test and both VM/C API weak
  `__newindex` target tests (`tests/t-gc2-traverse.c:1041`, `:3579`, `:3643`).
- The public raw-mutation recovery test above must remain unchanged. It tests
  the public barrier contract that this optimization deliberately does not
  relax.

The source edit adds a proof comment above `meta_tset_rooted_mode()` and guards
both pre-store table barriers with `if (!kept_roots)`. No GC handoff or public
root-barrier implementation was changed. `git diff --check` passed.

An isolated Linux build used archived HEAD
`eb77c111bcc14db3c92243b5e5c8860371814e59` with only the changed `src/lj_meta.c`
overlaid. GCC built the static runtime and fixtures with `-Werror`,
`LUA_USE_ASSERT`, `LJ_GC2_TEST_HELPERS`, `LJ_TAB_TEST_HELPERS`, and
`LJ_TG_ROOT_TEST_HELPERS`. The unchanged `t-meta-rooted-chain`,
`t-gc2-public-store-weak-window`, `t-gc2-table-store-guard`, and full
`t-gc2-traverse` all passed. No new fixture was added for this narrow elision.

An extra isolated run of `t-gc2-recovery` initially failed at
`test_grey_growth_transaction()` line 665 (`gc2_grey_bottom_acq(f.g) == 1`),
before its public-barrier regression ran. This was a pre-existing startup
queue assumption, repaired separately in `1981938f`; the complete recovery
fixture now passes against the combined current runtime and original-GC
controls. The public-barrier assertion was preserved. See
`notes/gc-recovery-fixture-review-2026-09-04.md`. The isolated pre-repair
snapshot manifest, exact compile/run commands, and logs remain under
`/tmp/lj-meta-prestore-linux-20260904-n0o9fp28/`.

## Combined Linux checks and measured cost

The combined runtime at `eb77c111` plus this `lj_meta.c` change also built with
GCC and `-Werror`, `LUA_USE_ASSERT`, `LJ_GC2_TEST_HELPERS`,
`LJ_TAB_TEST_HELPERS`, `LJ_TG_ROOT_TEST_HELPERS`, and
`LJ_API_ROOT_TEST_HELPERS`. Rooted chains, the public weak window, store guards,
full traversal, x64 rooted reads, and keyed CAS store fixtures passed. The API
handoff fixture passed after correcting its pre-existing test hook to observe
registry publication rather than the initial nil reservation. That original
failure also reproduces with both the old GC source and pre-elision metamethod
helper; its corrected fixture passes that control.

[Raw paired measurements](../bench/table-prestore-barrier-2026-09-04.csv) and
[metadata and hashes](../bench/table-prestore-barrier-2026-09-04.json) record
seven fresh-process before/after pairs. Both normal GCC runtime builds contain
the table and GC fixes at `eb77c111`; they differ only in `src/lj_meta.c`.
The measured command was:

```sh
BENCH_SCALE=0.05 taskset -c 30 "$binary" -joff \
  "$review_dir/before/plan/aux/bench/bench.lua" tab_insert_newkey
```

This runs 10,000 insertions per internal iteration and reports the minimum
process CPU time across five iterations. Odd pairs ran before/after and even
pairs reversed the order. All 14 processes exited zero with empty stderr.
Median reported cost was 2458.8 ns/op before and 2453.0 ns/op after; the
geometric mean of paired after/before ratios was 0.996825. This does not
establish a material speedup or address the large full-sequence insertion
cliff. CPU frequency was not fixed, and functional checks on CPUs 0-15 overlapped
the final measurement tail. Keep the change's narrow proof and the larger
performance work separate.

The isolated measurement trees, build logs and complete stdout/stderr remain
at `/tmp/lj-meta-barrier-review-my8sb8yp/`. Normal build flags matched the
baseline review: GCC 14.2.0, `-std=gnu11 -O2 -fomit-frame-pointer -mcx16`,
without assertion, sanitizer or test-helper flags.

The same normal after-build also passed the repository's default stock suite:
387 tests with `-joff`, and 509 with `-jon`. Both processes exited zero using
`test.lua --quiet` in `tests/stock/test`, with the runtime's Lua modules on
`LUA_PATH` and a 240-second outer timeout. Logs are
`/tmp/lj-review-stock-joff.log` and `/tmp/lj-review-stock-jon.log`.
