# Captured C-library receiver guards, 2026-09-05

A captured namespace `__index` or `__newindex` function could execute native
code specialized to the wrong receiver. A loop warmed with library A kept
reading or writing A when passed library B, and accepted an IO userdata where
the interpreter raises an error. Ordinary metamethod lookup's receiver guard
does not cover a directly called builtin.

`recff_clib_index` now compares its runtime receiver with an exact typed
userdata KGC constant before exporting a symbol, constant, or extern address.
The same check covers reads and writes. KGC retention keeps the namespace
alive through recording and installed traces; a raw address comparison would
not provide that lifetime. The specialized userdata subtype is published once,
so exact retained identity also establishes the expected subtype. This adds
one native equality guard and no helper, allocation, lock, or wait.

The permanent fixture builds two libraries with distinct extern values, then
tests root and installed-side receiver changes, wrong userdata types, reads,
writes, and namespace retention after full collection. It requires actual
native execution and exits from original retained traces. Preceding Lua writes
are counted exactly, including the throwing cases, to catch replay on exit.
Wrong-namespace stores must leave the original library unchanged.

All ten cases pass with JIT off and on in default, assertion, and Clang ASan
builds: 60 positive processes. The exact earlier baseline and the method-guard
fix without this receiver change each pass ten interpreter controls and fail
all ten native controls. The receiver patch is unchanged across these checks.
All 224 runtime/generator inputs match the fixed build variants; relative to
`9f68fa8d`, only `lj_crecord.c` changes. Its final SHA-256 is
`d9117ff3214f258cc84648725effcd498f3d1e93a774a94efd65e81ed11f2bff`.

ROOT's combined validation adds 54 passing runtime processes across the three
builds. These cover both stock modes (387 JIT-off / 509 JIT-on per build), all
11 namespace method-mutation cases, recorder metadata contention, extern
snapshots, cache retirement, callback stack relocation/unwind, and real remote
CALLXS pointer/bool/sret collection and flush. ASan/LSan uses
`detect_leaks=1:abort_on_error=1` without suppressions; target instrumentation
and uninstrumented host generators are verified. The shared canonical
`m7_ffi_clib_receiver` entry passes all 20 cases in 43.440 seconds including
default build preparation.

[The source review and isolated results](evidence/ffi-clib-receiver-2026-09-05/isolated/receiver-handoff.md)
and [combined validation](evidence/ffi-clib-receiver-2026-09-05/root/final-validation.json)
retain exact commands, input/binary identities, outputs, and terminal status.
The first side-trace fixture incorrectly required only the final-iteration
side to observe every error. Two resulting positive-test assertion failures
and their diagnostic are retained. The final fixture checks the frozen set of
all installed root-linked sides; no runtime change accompanied that correction.

Native cache lookup still needs runtime authority for debug environment writes
and semantic library close. These are independently reproduced baseline bugs,
not fixed by retaining the receiver. General shared-MT method recording and
its lifetime/progress contracts also remain open. This commit has no paired
performance or release-readiness claim; validation is Linux x64 only.
