# Dead CALLM expressions and early-break iterator integration

Production merge `c9480cfa8` combines the verified `0641fba1e` checkpoint
with `b6fe9a638` (dead numeric CALLM DAG proofs) and `9e7eea72c` (canonical
scalar iterator early-break roots). The subsequent `efc0b2a63` merge imports
checkpoint documentation only.

The combined source passes all four ARM64/arm64e assertion/release profiles
of both `arm64_jit_callm_dead_numeric_contract.sh` and
`arm64_jit_iterl_contract.sh`. Each contract restores an ordinary assertion
ARM64 build without test helpers. Logs are in
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/`:

- `dead-numeric-combined-four-profile.log`
- `early-break-combined-four-profile.log`

The restored ordinary runtime passes the dead terminal, chained arithmetic,
and division CALLM companion. Each selected root reports 63 native exits
across the retained-code GC check. The ordinary topology inventory now
publishes the scalar iterator runner root and observes its exits for normal,
bad, zero, short and longer inputs. Both stock corpora pass all 509 cases.
Evidence is in `callm-iterator-ordinary-dead-numeric.log`,
`callm-iterator-ordinary-topology.log`, `callm-iterator-stock-source.log` and
`callm-iterator-stock-bytecode.log` in the same directory.

The uninterrupted complete gate remains at `0641fba1e`; it does not yet cover
these two production additions. Independent review found the missing spill
and RENAME-specific fixture mutations worth strengthening; that test-only
follow-up is separate from these recorded results.

The inventory still exposes genuine gaps. In particular, the source
`values(x) return x,.25,.5` feeding `sum(a,b,c) return a+b+c` inside a numeric
loop still has no ARM64 runner root, while the preserved x64 run does. The
recursive Fibonacci and vararg-forwarding rows also remain different. A
successful interpreter companion or a different admitted CALLM shape does
not close these gaps.

## Pointer-argument and negative-test integration

`8106cfe53` adds the pointer-member aggregate argument change from
`5eef347e7` and the test-only spill/RENAME follow-up from `ec41af38c`. The
combined aggregate contract passes ARM64/arm64e assertion/release profiles
in `pointer-args-combined-four-profile.log`, then restores ordinary assertion
ARM64. Both ordinary ARM64 stock corpora pass all 509 cases in
`pointer-args-combined-stock-{source,bytecode}.log`.

The matching ordinary x86_64 build passes with `-Werror` and both 509-case
stock corpora. Its profile is
`0a80b1acb9e5369e890ecd583cf39979cd811503385471c1b1509101a82bfa22`;
logs are `callm-iterator-pointer-x64-build.log` and
`callm-iterator-pointer-x64-stock-{source,bytecode}.log`.

A new uninterrupted full gate is running at pinned source `8106cfe53` in
`ir-admission`, with output `callm-iterator-pointer-complete-gate.log`. Its
result is not yet claimed. The promoted continuation remains at the prior
verified production source `0641fba1e`; the transient CALLM window and newer
captured producer investigation remain separate work.

## Complete gate result

The uninterrupted full gate at `8106cfe53` passed. It ends with
`arm64_jit_fail_closed_gate OK` and executes both `t-vm-safepoint` and
`t-jit-recorder-safepoint`, followed by
`ok m5_arm64_jit_fail_closed_safepoint_runtime`. This includes all four M7
foreign-call profiles, the new dead-DAG spill/RENAME mutations, and the
earlier iterator/pointer-argument changes. No skipped safepoint runtime suite
is counted as execution.

The outer command then rebuilt ordinary assertion ARM64 without test helpers
and exited zero. The restoration profile is
`c3b83e733e7da77a4c6551c4271c5a2bb36051c05e91c3b24346954238ac4ed9`, recorded
in `callm-iterator-pointer-complete-restoration.log` beside the gate log.
The pinned `ir-admission` checkout is clean. This result supersedes the
pending-gate statement above; later deep-side-list and CALLM producer work
remain outside the gate.

## Independent continuation rebuild

After promotion, the `arm64-parity` continuation was independently rebuilt
from `a6041d77d` as ordinary assertion ARM64 without test helpers. Its profile
is `2d9dbcc06dcb6fb789f8fdba9ada4271793786675118049d9c5d36262542306d`.
Both stock suites pass all 509 cases. The dead-terminal, dead-chain and
dead-division companions each observe 63 selected exits, and the ordinary
multi-side topology probe completes. Logs are
`callm-iterator-pointer-main-{build,stock-source,stock-bytecode,dead-numeric,topology}.log`
in `/Users/frityet/Scratch/ljmt-side-ancestry-20261029/`.
This confirms the promoted build separately from the pinned full-gate build;
it does not include the later deep-chain or CALLM producer work.
