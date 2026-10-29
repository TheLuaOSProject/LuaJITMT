# Native children for independent FORL branches

Production source `6968c4cac` matched the independent two/three-branch loop
results but refused their ARM64 child traces. The ordinary x64 comparison
published one native child for each independent root exit. The ARM64 semantic
snapshot interval rejected cold continuations exactly at the parent FORL,
even though the existing child range-guard proof explicitly recognizes this
instruction as the correct retry point.

Both semantic and post-RA admission now accept this boundary only for a
root-linked child whose complete four control slots bind to the exact source
loop. IDX and visible EXT must be the same inherited pre-update INT load;
STOP must be the inherited read-only INT load; STEP must equal the source
KSHORT constant. All four slots must occur exactly once. The normal snapshot
flag, source-bytecode, parent binding, range-guard and allocated-location
checks remain active. The VM can retry the increment without skipping or
repeating the body that has already completed. No lock is added.

## Validation

`tools/ci/arm64_jit_forl_boundary_sides_contract.sh` passes ordinary ARM64
without test helpers and all four ARM64/arm64e assertion/release helper
profiles. The dedicated fixture uses three independent branches in ascending,
descending-with-variable-limit, and step-two loops. It requires three exact
children from the named kernel's prototype, distinct parent exits, root-linked
native bodies and exact exit targets. Repeated short/long calls must exit
from a selected child at most twice per call. Results match a separately
loaded interpreter oracle. Full collection and flush/reheat exercise each
case twice with a live attached peer.

Private copies pass both validators before mutations. Eleven independent
FORL negatives reject in both walks: the existing range guard, step, ordering,
index and retry-PC cases plus an advanced visible index, wrong stop, wrong
step constant, missing visible index with a well-formed compacted map, and an
invalid NORESTORE visible index. Other copied-side tests retain their old
masks when these boundary mutations do not apply.

The separate ordinary two/three-branch topology probe now publishes two and
three native children respectively, with later calls producing child exits
rather than a root exit on every iteration. Diagnostic and clean prototype
logs, source copies, runtime/archive and contract logs are retained in
`/Users/frityet/Scratch/ljmt-forl-independent-sides-20261029/`. The final
five-profile log is `boundary-contract-all-profiles.log`; earlier
`boundary-contract-first.log` and `boundary-contract-diagnostic.log` preserve
a fixture extension that exposed a different gap: `for i=n,1,-1` uses a
constant stop and still fails child admission. The tested descending case
uses `for i=64,n,-1`, whose stop is inherited. Constant-stop child support is
follow-up work; this checkpoint does not claim it or whole-fork parity.

The full gate now includes the focused boundary contract. The full integrated
gate is a separate validation step; the profile pass above does not claim it.
