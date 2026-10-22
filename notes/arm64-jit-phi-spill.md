# ARM64 PHI spill-copy entry boundary

2026-10-22. A capability-level admission experiment exposed a real ARM64
code-generation bug before any production gate was widened. The fix is one
assignment in common `asm_loop()`; the ARM64 emitter itself stays unchanged.

## Failure and mechanism

`tests/t-arm64-jit-phi-pressure.lua` keeps 36 numeric recurrences live in a
self-linked loop. A disposable diagnostic admission prototype produced 21
numeric spilled IR values and a 128-byte stack adjustment. With the original
assembler, only accumulator 5 was wrong: `3.2388853905016` instead of the
interpreter's `3.0534774712331`. A repeat call had one observed native root
publication and one native exit. The same Lua reproducer fails on the installed
unmodified Homebrew LuaJIT `2.1.1788856981`.

`asm_loop()` records `mcspill` after PHI register shuffling. A prologue branch
skips the PHI spill-copy region and enters the ordinary body at `mcspill`.
While emitting that copy region backwards, `emit_lso()` merged its last
`STR d0, [sp, #56]` into the existing body `STR d1, [sp, #48]`, creating
`STP d1, d0, [sp, #48]`. First entry consequently stored stale `d0` without
executing the corresponding copy-region load. Backedges did execute the load,
but could not repair the corrupted first-iteration value.

The existing ARM/ARM64 peepholes already stop at `as->mcloop`. Publishing
`as->mcloop = mcspill = as->mcp` before `asm_phi_copyspill()` fences this second
entry point; the existing final assignment still records the backedge entry.
No extra machine instruction or general pairing disable is needed. Other
backends do not inspect this field during the affected emission interval.

## Validation

- Disposable arm64e/BTI capability experiment: every accumulator now matches
  the interpreter exactly, including a second observed native call with the
  same 128-byte frame and 21 numeric spilled IR values.
- `lj_asm_arm64_phi_spill_test()` calls production `asm_loop()` and
  `asm_phi_copyspill()` with adjacent copy/body stores. Its emitted function
  checks first-entry preservation and backedge copying for 1–100 iterations.
  It is compiled only under existing `LJ_ARM64_EMIT_TEST_HELPERS` and does not
  alter trace admission.
- `tools/ci/arm64_jit_phi_spill_contract.sh` executes the emitted code and
  verifies same-block loads still pair. Native arm64 and arm64e runs pass.
- Removing only the boundary assignment in the disposable copy makes the
  generated-code fixture fail its first value assertion (exit 134).

The standalone Lua pressure test reports SKIP while production NUM-spill
admission remains closed. That skip is not a spill correctness result: the
isolated emitted-code fixture supplies the production-policy-independent
regression. The disposable permissive admission prototype is not committed.
