# ARM64 NUM FORL direction binding

This checkpoint adds a bounded NUM `FORL` root path alongside the existing
INT `FORL` path. The semantic IR walk and the post-RA walk independently prove
the floating index recurrence, stop/step source slots, direction comparison,
ordered loop guards, PHI linkage, register allocation, and snapshot shape.
They return the source-proven direction separately; assembly requires the two
walks to agree before publishing the trace admission bits. NUM admission is
split into ascending and descending values rather than one untyped capability.

The ARM64 VM computes the NUM update and ordered loop test as before. At the
taken `JFORL` edge it passes a compact mode derived from the step's sign bit:
integer, ascending NUM, or descending NUM. The helper acquires the existing
GC2/TG lifetime lease before reading trace metadata, rejects a mode mismatch,
and clears the lease before returning. It does not transfer to native code on
mismatch. A finite cross-direction call verifies both ascending-root to
descending-input and descending-root to ascending-input rejection, then checks
the TG has no native state or `jit_base` left behind.

The focused fixture also compares `+0.0`, `-0.0`, infinities, and NaN inputs
against the same C-input JIT-disabled Lua oracle. The signed-zero bounds are
chosen so the interpreter terminates; the `-0.0` taken-edge case is not claimed
as a native witness because it would require an extra bounded-loop control
shape that this FORL proof intentionally rejects. Native direction-transition
witnesses use finite positive and negative steps.

Validation:

- The current assertion `arm64_jit_forl_record_contract.sh` passed on ARM64 and
  arm64e, including repeated native calls and the mcode-test run. It includes
  independent private semantic and post-RA negatives that change both the
  pre-loop and post-loop NUM bounds to another valid NUM SLOAD:
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-record4.log`.
- The same contract passed ARM64 and arm64e without `LUA_USE_ASSERT`, with one
  native fixture run per target:
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-record-release.log`.
- The native VM contract passed the integer and signed-mode NUM JFORL paths on
  ARM64 and arm64e:
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-native2.log`.
- Existing root-entry, scalar FUNCF, and ITERL direct-call regression
  contracts passed after all direct C callers were supplied mode 0:
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-root-entry.log`,
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-funcf.log`, and
  `/Users/frityet/Scratch/ljmt-forl-final-20261030-iterl.log`.
- A strict x86_64 `-Wall -Wextra -Werror` compile of `lj_asm.c` and `lj_trace.c`
  passed. The generic IR admission contract also passed in both FORL record
  profiles.

The earlier ambiguous attempt at this contract wrote to a shared Scratch log
while another worktree could be running a similarly named job; its output is
not used as evidence. Final commands explicitly set `LJ_TEST_ROOT`, verified
the real worktree path and lock owner, and used unique log/TMPDIR paths.

This does not claim unrestricted floating-loop parity. Zero/NaN constant-step
roots, other loop source shapes, and NUM side-loop admission remain closed.
