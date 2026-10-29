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
