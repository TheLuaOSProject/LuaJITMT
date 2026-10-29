# ARM64 disabled FUNCF hotcount patch ordering

An unsupported fixed-function root can be disabled with `jit.off(fn, true)`
while its entry opcode remains `BC_FUNCF`. The interpreter still routes its
hot call through `lj_trace_hot()`. Before this change, ARM64's root source
preflight rejected that unsupported bytecode in
`trace_arm64_root_recorder_preflight()` before `trace_start()` reached its
existing `PROTO_NOJIT` lazy patch. Repeated calls could therefore keep
triggering that hashed call bucket. Moving only the `trace_start()` source gate
was insufficient: the earlier recorder preflight was the actual blocker.

The recorder preflight now lets a `PROTO_NOJIT` function proceed only when the
captured bytecode is `BC_FUNCF` at the exact prototype entry. This is not trace
admission: `trace_start()` takes its existing no-JIT path, atomically changes
the entry to `BC_IFUNCF`, marks `PROTO_ILOOP`, and returns idle. JIT-enabled
functions still require the complete source-generation proof in both
preflight and `trace_start()`.

The C contract adds `test_disabled_unsupported_funcf_hotpatch()`. It creates
an unsupported equality function, disables it, sets the exact `bc+1` hashed
hotcount bucket to one, verifies the first call reaches zero without patching,
and verifies the next underflow patches the entry. Eight subsequent calls
leave the bucket unchanged and no trace 1 is published. Existing enabled
FUNCF positive and malformed-source rejection cases remain in the same
contract.

Validation evidence:

- `tools/ci/arm64_jit_funcf_record_contract.sh` passed all default/randomized
  ARM64 and ARM64e profiles. Clean log:
  `/Users/frityet/Scratch/ljmt-nojit-hotcount-20261029/focused-clean-contract.log`.
- The topology probe source SHA-256 is
  `595f5997a657d6da6bb129a5e484ae7a2dd8c54ae6be075ea8296b929a9c94f7`.
  Five runs each on the isolated current ARM64 ordinary runtime reported 81
  selected-root exits for `hot-function-root`; five runs on a current strict
  x86-64 ordinary build reported 5 selected-root exits. All ten probe runs
  exited successfully. Runtime bundles and logs are under
  `/Users/frityet/Scratch/ljmt-nojit-hotcount-20261029/arm64-current/` and
  `/Users/frityet/Scratch/ljmt-nojit-hotcount-20261029/x64-current/`.
- The x86-64 ordinary build used Darwin x86-64, `-mcx16`, and
  `XCFLAGS='-DLUA_USE_ASSERT -Werror'`; it completed successfully. The ARM64
  runtime bundle came from the clean restoration build of the four-profile
  contract. Both bundle hashes are recorded in their `binary.sha256` files.

The older frozen ARM64 baseline runtime at
`/Users/frityet/Scratch/ljmt-callm-pair-review-20261029/baseline-runtime/`
was not an x86-64 executable. The older x86-64 runtime from
`/Users/frityet/Scratch/ljmt-topology-probe-20261028/x64/` also ran the current
probe five times, but the matched current-source x86-64 build above is the
relevant architecture comparison.
