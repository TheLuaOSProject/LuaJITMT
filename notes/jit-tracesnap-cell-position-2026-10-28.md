# Snapshot positions across local-cell bytecodes

`jit.util.tracesnap(trace, snapshot, true)` found a snapshot's owning function
by scanning backward while the opcode was less than `BC_FUNCF`. LuaJITMT
appends `CNEW`, `CGET`, and `CSET` after the function headers, so this scan
stopped at a local-cell instruction and reported a truncated position. A
small-aggregate loop's completion snapshot reported position 0 instead of 14.

The scan now uses the existing `bc_isfunc_or_ff()` predicate. It retains the
same trace read-token/SMR interval and takes no new locks or allocations. The
change applies to both architectures. Snapshot storage and execution are not
changed.

The existing `t-jit-util-tracesnap-getpos.c` test previously copied the same
opcode-order scan as its oracle. It now keeps a known scalar workload closure
rooted, compares snapshot PCs directly to that prototype's bytecode base, and
requires a snapshot beyond a local-cell instruction. It still checks omitted,
false, true, and truthy-zero third arguments, plus safe refusal during exclusive
metadata reclamation and token/SMR cleanup.

On production base `3e3f3432a` plus this change, the C test passes with
assertions on native ARM64 experimental JIT and x86_64/Rosetta. The aggregate
probe also passes on both builds; x86_64's selected completion exit now reports
position 14 (`CGET`) instead of 0. Replacing only the library implementation
with the pre-fix `lib_jit.c` makes the strengthened ARM64 test fail at the exact
position comparison. This counterfactual confirms the test detects the bug.

Evidence is under
`/Users/frityet/Scratch/ljmt-aggregate-native-probe-20261028/`:
`getpos-check.sh`, `getpos-combined.log`, `getpos-{arm64,x86_64}.log`,
matching executable `.profile` records, `getpos-counterfactual.log`, and
`{arm64,x86_64}-fixed.tsv`. The initial manual build lacked the deployment
setting and did not run a test; the profile-helper runs above supersede it.
