# Literal integer prefixes with variadic aggregate returns

The folded 64-bit argument proof from `9a14875cb` also closes the original
literal-prefix composition: eight Lua integer literals occupy fixed U64
GPR arguments, followed by a named integer aggregate on the stack, another
fixed scalar, promoted anonymous CData arguments, and an indirect 24-byte
aggregate result. No additional production change was needed.

The permanent variadic fixture now exercises this shape alongside the
captured-CData prefix. It requires observed native execution, exact C-call
counts and returned fields, then full GC and reuse of the selected trace with
no leaked native lease or pin. The complete variadic contract passes all
four ARM64/ARM64e assertion/release profiles at base `af05ea408` plus this
fixture addition. It restores ordinary assertion ARM64 without test helpers.

An independent ordinary probe compares `9a14875cb` ARM64 with the preserved
strict x86_64 `ec98dbf5a` executable. Both select root 1 and observe eight
root exits before and after GC, with identical aggregate results and exactly
192 C calls in each eight-invocation phase. Libraries were built from the
captured-prefix fixture with the restored positional argument checks.

Evidence is under `/Users/frityet/Scratch/ljmt-ffi-int-u64-20261030/`:
`literal-prefix-probe.lua`, `{arm64,x86_64}-literal-prefix.log`, and
`literal-prefix-four-profiles.log`. These are focused composition checks;
the separate full gate and full x86_64 parity remain outstanding.
