# Cdata `__call` recorder/interpreter selection race

The recorder sampled a callable cdata `__call` entry, then the owning interpreter
looked the mutable entry up again after a peer changed it. A C-to-Lua replacement
made the recorder continue with the C sample while the VM executed Lua; the
preserved baseline aborts in `rec_check_slots` (`slot 8 const mismatch`). A
second deterministic case showed the reverse Lua-to-C replacement. Replacing
`__call` with a number after a C sample also reaches the ordinary call error
path, so pending recorder state must be cleaned before the error unwinds.

The fix stores an owner-only pending expectation at the recorder's shared cdata
call boundary. `lj_meta_call` checks it after the interpreter performs its
actual lookup and before the VM shifts arguments or installs a callee. It first
requires the current Lua state to hold the JIT token and be the published owner;
then it binds the pending sample to the caller prototype, exact call PC, and
function slot. The allowed identity is taken from the specialization actually
returned by `rec_call_specialize`: exact closure identity, Lua prototype, or the
existing special FFID case. A mismatch aborts recording and lets the interpreter
continue with its own selected value or normal error. The VM dispatch and stack
layout are unchanged. Pending state is reset on new recording and terminal
release.

A test-only one-shot pause uses the existing metatable-capture test gate between
recorder sampling and VM lookup. The C harness lets another TG perform ordinary
`rawset` at that seam. Tests prove C-sampled/Lua-executed and Lua-sampled/C-executed
results, the nonfunction call error under `pcall`, and fresh native `CALLXS`
publication after flushing and restoring the stable C method. The original
randomized direct-Lua stress path remains in the M7 ARM selector alongside the
deterministic harness.

Evidence:

- The original failing binary/runtime/DSO hashes and exact deterministic failure
  are preserved in `/Users/frityet/Scratch/ljmt-remote-flush-20261029/deterministic-before-fix-hashes.txt`
  and `/Users/frityet/Scratch/ljmt-remote-flush-20261029/deterministic-before-fix.log`.
- Native ARM64 assert helper build and both the deterministic harness and
  ordinary randomized script passed. Logs:
  `/Users/frityet/Scratch/ljmt-remote-flush-20261029/metacall-final/arm64-deterministic.log`
  and `/Users/frityet/Scratch/ljmt-remote-flush-20261029/metacall-final/arm64-ordinary.log`.
- x86_64 assert and release profiles were cross-built and run under the
  available macOS environment; the deterministic harness passed both.
  Profile hashes and output:
  `/Users/frityet/Scratch/ljmt-remote-flush-20261029/metacall-final/x64-final.log`.

This is evidence for the shared cdata call recorder/interpreter boundary and
these explicit mismatch directions. It does not broaden admission for other
metamethods or claim the complete M7 gate has run on this branch.
