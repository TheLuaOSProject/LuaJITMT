# Empty reclaimed arena certificate and reuse evidence

The final design and validation audit is
`notes/arena-empty-reclaimed-reuse-2026-09-05.md`.

`reuse-v2/` contains the final separate-atomic-head candidate, its matched
runtime-input manifest, final build/fixture/TSan results, settled accounting
control, layout probe, and all 20 primary performance processes. Every final
process completed. `performance-summary.json` derives from the raw stdout;
the original harness reports best-of-five inner rounds, while the surrounding
script runs three fresh filtered pairs and seven fresh post-insertion pairs.

Root-level performance files and `negative-v1-*` identify the rejected first
revision. It was faster but retained 329→853 arenas across five closure rounds.
Its deterministic generic-allocation and typed-pair controls each chose a fresh
mapping despite two available real spares. The final revision keeps exactly four
arenas across 32 cycles of each control and 320 across all five real closure
rounds. The proposed interior-link/tail scheme was rejected before implementation
because it would race concurrent GC publication and owner adoption.

Both A/B revisions compare against the same immutable normal baseline,
`/tmp/lj-gc-jit-combined-20260905-6cxpl6mp/normal`. It predates the final public
MARK-scope repair and later scalar-read changes. Every tracked runtime input
matches each candidate except arena C/header. Local paths and exact binary/source
hashes are in the metadata; the temporary trees and binaries are not committed.

All measurements keep GC enabled and JIT on, pin processes to CPU 30, alternate
the order within fresh pairs, and preserve a 45-second process bound. CPU 30 has
no SMT sibling and uses the performance governor; frequency is not fixed. Other
agents ran functional work on CPUs 0–15. Own validation overlapped the diagnostic
pair but had ended before the final primary pairs.

`gcdiag.c` is a separate native frontend linked against each unchanged static
archive. It reads allocator lists only from the main owner with no GC workers or
MT execution, and snapshots outside timing. The settled harness adds an explicit
collection after each closure round so retained physical capacity can be checked
alongside live-byte and cycle counters. The final frontend counts both reclaimed
heads. Command arrays, environment, exit status, wall/CPU time and partial output
are retained in the JSON records. Copy evidence before rerunning the scripts,
which write their output beside themselves.

The initial standalone build's runtime-assertion-helper link error and a later
new-fixture HugeTab initialization failure are retained with their corrections;
neither is omitted from the audit. TSan demotes only GCC's known unsupported
atomic-fence instrumentation warning. No host settings were changed.
