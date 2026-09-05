# Combined Linux stability checks, 2026-09-05

The final empty-arena reuse and scalar table-read changes pass together with
the existing GC coalescing and deferred JIT retirement work. All 30 recorded
functional processes completed successfully. This is Linux regression and
protocol coverage, not completion of the lockless or release acceptance gates.

The frozen tree starts at
`d680421c4cb50b85437d88255bc89358c5e3a6b1` and overlays the exact production
files subsequently committed as `bec8cd2d` and `23c0c753`. The normal executable
SHA-256 is `4914c147ac254179610c3aeb8ba847fdc215febcbb96af81afb365487b2a1c37`;
the helper/assert executable is
`959115a356730c488bf4067fa78f811fd5ea31a2e47377a809cb643fad7e3e21`.
Every tracked source and DynASM input in the normal tree was subsequently
confirmed equal to production commit `23c0c753` before performance measurement.
The rejected growing-capacity allocator and wide-stamp experiment are excluded.

The batch includes 18 strict helper/assert C fixtures covering allocator
statistics, empty-spare reuse, GC traversal/recovery/publication/coalescing,
weak stores/resizing, JIT abort retirement, scalar/rooted reads, worker
scheduling, and terminal orphan reclamation. Two normal C fixtures cover
thread lifecycle and unchanged allocator state churn. The latter completes
in 5.379 seconds; this is a functional completion observation, not an isolated
before/after speedup measurement.

Normal stock suites pass 387 tests with JIT off and 509 with JIT on. Eight
normal Lua processes cover the 15 general resize cases, native store/read/
iterator resize, weak/finalizer JIT overlap, remote native stack GC, string
interning with collection, both buffer modes, and JIT trace pressure. Each
process has an explicit finite limit and its actual exit is recorded.

All final arena and scalar fixture changes are included. The JIT resize
fixture is the measured controller-observer revision; a subsequent comment
and assertion-wording-only edit was independently checked with one complete
positive process and four required negative controls. Its evidence is in
`notes/jit-resize-native-exit-coverage-2026-09-05.md`.

`notes/evidence/linux-integrated-stability-2026-09-05/` preserves the source
manifest, build commands/logs, validation driver, every process result and
stdout/stderr. Original immutable build trees and executables remain under
`/tmp/lj-linux-integrated-stability-20260905-st7b2_hh`. This combined batch has
no sanitizer instrumentation; isolated ASan and allocator-only TSan coverage
are documented with their respective changes. Windows and macOS work remains
deferred until preparation of the next release, per the user's direction.
