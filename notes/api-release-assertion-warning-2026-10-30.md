# Assertion-only API temporary in release builds

`api_vm_prepare_entry_claimed` records `topofs` for its error-value assertion.
When assertions are disabled, Clang reports an unused-but-set variable and
strict release builds fail. A single `UNUSED(topofs)` annotation retains the
existing assertion and has no effect on runtime ownership or unwind behavior.

The warning was reproduced with an x86_64 release compilation of `lj_api.c`
using `-O2 -Wall -Wextra -Werror`. After the annotation, that translation unit
compiles with those flags for all four ARM64/x86_64 assertion/release
combinations, with the ARM64 bootstrap/experimental flags on ARM64.

Evidence: `/Users/frityet/Scratch/ljmt-release-api-warning-20261030/` contains
the failing `before.log`, four successful compilation logs and objects. This
is a compile-only validation of an annotation; no new runtime claim is made.
