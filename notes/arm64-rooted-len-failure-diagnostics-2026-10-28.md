# Rooted table-length failure diagnostics

The rooted-length native witness keeps its existing strict requirements for a
published root entry, an exit-handler call, and execution of
`lj_tab_len_rooted_try`. The helper-count requirement now shares the
failure-only diagnostic path, so a missing helper call reports the same context
as a missing publication or exit.

On those failures, `tests/t-tab-rooted-len-try.c` prints the helper count,
entry publication/success/cleanup/start-instruction counts, exit count, GC2
phase and JIT gate, TG request/poll/flags and VM state, native depth, JIT base
and activity, SMR readers, root anchors, table-read depth, and root descriptor
state. The existing bytecode PC diagnostics remain. Passing runs stay quiet.

This is test-only reporting. It does not change runtime behavior, relax a
native witness, or extend any deadline. The prior post-full-GC failure remains
unexplained; these diagnostics are intended to preserve useful evidence if it
recurs.
