# Deeper side chains and shallow list validation

An ordinary 16-branch while probe at `8106cfe53` published only four ARM64
bodies (root plus depth three). Later branch modes repeatedly exited from the
last side, including 3,071 exits over 32 calls after GC. The matching x64
runtime published deeper parent chains and returned the same oracle values.
Its deepest row includes an exit trace and had no selected-exit observation;
that row alone is not a deepest-native-entry claim.

Diagnostics localized the ARM64 refusal to acquisition of the root's view.
`trace_arm64_first_side_view_acq_mode` visits every node in the root-owned
list. When it visits a depth-two child, it used the direct-parent membership
helper, which validates the entire list under a depth-two assumption. An
already-published depth-three descendant therefore prevented the next child
from starting even though the bounded indexed graph proof accepted the graph.

The direct-parent branch now retains its exact parent certificate check and
the existing shallow fast path. If that list proof cannot validate, it uses
the same indexed whole-graph proof already used by deeper parent branches.
Once checked, that graph is reused for the rest of the scan. Every outer-loop
member and immediate parent is still independently required runnable and
certificate-valid; the complete list, root tuple and selected target are
rechecked before success. The retirement helper itself is unchanged.

The ordinary-source diagnostic probes now publish all 16 and 64 branch
bodies, with the deepest ARM64 side observing 32 exits in 32 post-GC calls.
Evidence is under `/Users/frityet/Scratch/ljmt-deep-chain-audit-20261029/`:
`probe.lua`, `arm64-16.log`, `x64-16.log`, `diagnostic3-8.log`,
`candidate-16.log` and `candidate-64.log`. The candidate diagnostics used an
assertion runtime with test helpers; an ordinary rebuild remains a separate
check. Temporary diagnostic prints were removed from the production diff.

The expanded C fixture builds an integer chain of depth 15 and a fractional
chain of depth 64 (the existing graph's full 64-child capacity). It validates
every actual parent exit target, independent copied ancestry negatives,
same generation/address/size/code bytes across GC, selected deepest native
exits with a live peer, and full flush followed by new-generation native
reentry. The assertion fixture passes in `deep-fixture-flush.log`.
The side contract subsequently passed all four ARM64/arm64e assertion/release
profiles, including the expanded depth-15/depth-64 fixture, the existing
capacity and teardown fixtures, and restoration to ordinary assertion ARM64.
Evidence is `four-profile.log`; the restored profile is
`7e8512ca2ecabe637fa3c0f07e61a133c4a401e35c0113be88f32693c673663e`.
The ordinary no-helper runtime then published all 65 bodies (depth 64) and
observed 32 deepest-side exits over 32 post-GC calls in `ordinary-65.log`.
Both stock corpora pass all 509 cases in `ordinary-stock-source.log` and
`ordinary-stock-bytecode.log`.

This does not increase the graph capacity or widen side source/IR admission.
Full x86_64 parity remains incomplete.
