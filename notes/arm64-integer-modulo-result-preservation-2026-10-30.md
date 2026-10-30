# Retained integer modulo results across loop calls

At `29eb99e92`, a loop that accumulates an INT modulo result compiles, but
retaining that result in another local adds a PHI that post-RA refuses. The
captured allocated graph is exact: initial MOD 32775 is in X0, LOOP 32781,
body MOD 32784 in X0, output PHI 32791=(32775,32784), and RENAME 32792 maps
the initial value at loop snapshot 3 to preserved X27. The call-survival check
was judging the initial X0 allocation despite the exact loop rename.

The existing numeric CALLN preservation proof now also handles a matched
INT MOD-to-MOD pair. It still requires one loop snapshot, one exact snapshot
RENAME, exactly one later SSA use as the PHI incoming operand with the body
call as its backedge, and the resolved location matching the RENAME in a
preserved register with no spill. Existing per-exit snapshot checks remain
independent. The NUM CALLN case retains its original restrictions. There is
no emitter change, lock, or blanket PHI liveness exemption.

An independent review found no concrete unsound acceptance in this extension.
The assertion profile passes direct corruption tests for volatile rename,
wrong snapshot/source, wrong producer/body type, an extra ordinary use, and
wrong PHI edges. The selected real trace discovers initial/body MOD, exact
PHI, loop snapshot, and preserved-GPR rename rather than assuming fixed refs.
The ordinary result-pair probe now publishes all four constant/dynamic
FORL/while cases and matches the existing signed-limit, zero-divisor,
overflow, coercion, GC and code-identity oracles.

Evidence is under
`/Users/frityet/Scratch/ljmt-next-numeric-primitive-inventory-20261030/`:
`result-phi-diagnostic.log`, `result-phi-development.log` and
`result-phi-assert.log`. The assertion contract restores ordinary ARM64 at
`e63632b62dfa49461cc9e874b53ae6cfd269b060d6cdbc14e76633f286dd81e5`.
Remaining profiles and fresh ordinary x64 validation are pending at this
source checkpoint; this does not claim full parity or a complete-gate pass.

## Four-profile and ordinary validation

Frozen source `72f03e07a8050ce2c034d89ed9340556d402628b` passed the complete integer-modulo contract on arm64/arm64e assertion and release profiles, including both accumulator and retained-result companions and independent allocation mutations. Evidence: `/Users/frityet/Scratch/ljmt-modulo-result-phi-final-20261030/four-profile.log`. Contract restoration fingerprint: `e7a550c8f605dc720990eb955994b7116374dbc5112f77cab8d05cb4ffe1b19c`.

Fresh ordinary ARM64 and strict `-Werror` x86_64 builds each passed the 509-case source corpus and the 509-case stock-bytecode corpus, both modulo companions, and the 15-row numeric inventory. The retained-result FORL/while and constant/dynamic cases kept their selected native roots, exited those roots, matched edge oracles, and preserved code across GC. x86_64 uses its existing NUM modulo graph; these counts do not compare performance. Matching runtime/module bundles and logs are saved under `/Users/frityet/Scratch/ljmt-modulo-result-phi-final-20261030/`. Final ordinary ARM64 restoration fingerprint: `99ca11da808bdd041e333b148ff8aaf3bcd1b5264a7c2785d3f36a48b977b0f6`.
