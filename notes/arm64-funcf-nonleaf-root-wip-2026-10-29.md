# ARM64 nonleaf FUNCF root prototype (WIP)

This checkpoint explores the x64 `nonleaf-function-root-return` topology in
`bench/jit-topology-probe.lua`: a fixed function root reads one captured leaf,
calls it with one fixed argument, performs a scalar continuation, and returns
one result. The experiment is limited to one `BC_UGET`, one `BC_CALL` with
`B=2/C=2`, and a `BC_RET1`. It does not cover tail calls, multiple results,
protected calls, recursion, or open arguments/results.

The recorder-side grammar and source-only dispatch were prototyped, and a
profile build of the existing `arm64_jit_funcf_record_contract.sh` passed for
ARM64 and ARM64e before generic root-call admission was attempted. A native
live-peer probe confirmed that the relevant UGET lowering uses
`TMPREF -> CALLS lj_func_getuv_rooted_try -> NE -> VLOAD FUNC -> EQ`, followed
by the child and caller arithmetic. The no-peer probe instead showed ordinary
`UREFC/ULOAD`; that shape is not representative of the lockless MT target and
is not included in this slice.

The first generic FUNCF admission experiment fails closed at the inline
child-frame snapshot. It has no native publication or native-entry witness,
and the independent post-RA root-call path and mutation contract are not
implemented. Consequently this checkpoint does not establish nonleaf FUNCF
parity. The remaining work is an exact no-loop caller/child frame and return
proof, followed by independent post-RA validation and native tests with a live
MT peer, identity changes, child type/overflow exits, GC, and trace reuse.

Evidence: existing scalar FUNCF profile log was produced by
`tools/ci/arm64_jit_funcf_record_contract.sh` in the isolated worktree. The
active-peer diagnostic IR and snapshot logs are in `/tmp/funcf_root_peer_ir.log`
and `/tmp/funcf_root_peer_snap.log`; these are exploratory diagnostics, not a
passing contract. The branch includes no claim that the target trace publishes
or enters natively.
