# Source-only foreground SWEEP checkpoint

The request-retention mechanism can be made small, but its service/payment contract is not yet implementation-ready. This package contains the accepted source and a concrete call/state design; **no runtime patch, build or runtime validation** was performed.

Read [DESIGN.md](DESIGN.md) for exact source locations, a restricted cycle-tagged request protocol, under-claim outcome requirements, STOP/FINPAUSE/worker/cycle transitions and five remaining implementation decisions.

The useful new conclusions are:

- A native-only publisher can use its existing JIT intent to pin the cycle until an owned acknowledgement. A separate nonzero `gc2.cycle` request word is plausible; cycle authority saturates rather than wraps. `jit_sweep_displaced` is not suitable because it also serves generated failed-entry fairness.
- The current positive drain result mixes graph transfer, retry, suffix handshake, grace, arena and finalizer work. Only exact service produced under the same worker claim can pay the obligation. SSB consumption alone does not establish rescue completion.
- A lease/claim refusal should end the automatic invocation with debt retained, followed by the existing ordinary mutator continuation. TNEW, TDUP and fast-function GC return paths already continue after the GC check. An early incorrect TNEW label inference was explicitly corrected; it is not evidence of a retry loop.
- Worker count publication precedes actual pthread startup and remains positive during stopping/failed join. It cannot acknowledge or erase unpaid work. Adding a new last-worker publisher would require extending the restricted ownership proof.
- The unresolved decision is how much native allocation a committed service prefix buys, or whether to use a complete graph/cycle endpoint. A fixed larger batch or threshold exit alone does not prove the retained recovery backlog is serviced. Full-cycle interpreted fallback is concrete but changes native fairness materially.

Source identity:

- Accepted origin: `/tmp/lj-worker-bridge-combined-20260905-bz9wysjp/candidate`.
- Runtime: `eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd` plus worker candidate2.
- This package's 225-file `base/` copy matched the accepted tree and sampled shared runtime; sampled shared HEAD was `6490b1273290ce9d2411419c1e02920bfa777975`. See `setup.json` and `source-verification.json` for exact identities. No shared runtime edit was made.
- Accepted `src/lj_gc2.c` SHA256: `3571a3f2128114c475a730fcb35a413cffdfd27124ba2dcb9e136d9690edbea9`.

By freeze, ROOT had integrated the worker change as `997c0044` and the independent scalar-next change as `4ab0a6bddcfe4654ab8beb1ceabe2067d78f65c8`. The final read-only audit records the later shared identity separately: only `src/lj_tab.c` and `src/lj_tab.h` differ from this accepted-source snapshot. All GC source in the checkpoint remains exact; the 225-file copy was not rebased or rewritten.

The earlier diagnosis remains immutable at `/tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u` (manifest SHA256 `407576f5b20f575a802918e517605a9f18522293fc9532d75ff170b0d54d416b`). That diagnosis uses the exact **eb8 baseline** tree `/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline` and its `src/libluajit.a`, SHA256 `cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343`. It is a distinct binary/source generation from this accepted worker+fair source checkpoint. Its unchanged fixed-bound failures, debugger perturbation qualifications, 50 actual hard handoffs, compiled instruction evidence and 258143 independently counted recovery identities are preserved; no new runtime outcome supersedes them.

`artifact-manifest.json` freezes this package. There is no candidate tree, patch, executable, modified fixture or changed test expectation here.
