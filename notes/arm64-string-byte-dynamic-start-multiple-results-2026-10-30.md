# Multiple byte results with dynamic positive bounds

This extends the fixed-result and dynamic-end byte checkpoints to calls such
as `local a,b = string.byte(s, first, last)`, where both bounds are integer
parameters or Lua numbers truncated by the normal string library rules.
The fixed CALL result window must fit the source frame. Each byte independently
proves `0 <= base < end <= length`, where `base = first-1`, and a nonnegative
displacement strictly smaller than the checked `end-base` width. Since the
result remains below `LJ_MAX_STR`, adding the displacement cannot wrap.
Semantic and allocated-state validators derive this separately.

Stock ARM64 `asm_fusexref` folds an adjacent `ADD(base, displacement)` and
`STRREF` into its byte load. The first attempt already passed both semantic
checks but failed the final allocated-state check because that ADD had no
register or spill. The new allocation exception matches this exact fusion:
an adjacent unallocated STRREF, a checked base with an actual location, the
independent dynamic range proof, and a displacement plus GCstr header smaller
than LDRB's 4096-byte immediate limit. The emitter is unchanged. Temporary
failure diagnostics were removed before validation.

The fixture covers widths 2, 4, 8 and 16, starts 1 and 2, and integer or
fractional first/end pairs. It compares weighted NUL/high-byte results and
normal error behavior with an interpreted oracle before and after GC. Cold
inputs change the result width, use zero/negative/extreme bounds, NaN and
infinities, shorten the string, or supply a non-string. The selected native
root must enter and exit, and its machine code must remain unchanged.

Every byte pointer and readonly load is mutated independently. The last
displacement is proved valid with a fresh private constant, then increased
to exactly the guarded width and rejected by both validators. Width-guard
mutations are also rejected. Independent allocation negatives remove the
base location or materialize the STRREF while leaving its ADD absent. Raw
U8 snapshot restoration remains outside this checkpoint.

## Validation

All four assertion/release ARM64/ARM64e profiles passed the complete byte
contract, including the added cold-bound vectors. The final ordinary ARM64
restoration profile is
`534297958f770593d14a949a9a2f3c20ee880e709f9c4280d6fc512b9f9a4487`.

Ordinary ARM64 and strict bare-`-Werror` x86_64 each passed all 509 stock
source cases and 509 original v2-bytecode cases. Their matched ordinary probe
passes forty rows: widths 2, 3, 4, 8 and 16, starts 1 and 2, and all four
integer/number combinations for the bounds. Each row observes eight selected
root exits before GC and eight after GC, with matching results and errors.
The x86_64 run restored ordinary ARM64 afterward; its restoration profile was
`df2a39763884be53b69227c2f3bb2cbf3768f9d1b20380cd9df6316ae25bc06d`.
The final added cold vectors change only the C fixture after that ordinary
matrix; the production implementation is identical.

Evidence is in `/Users/frityet/Scratch/ljmt-expanded-parity-20261030/`:
`dynamic-start-multiple-cold-four-profiles.log`,
`dynamic-start-multiple-mixed-ordinary.lua`,
`dynamic-start-multiple-mixed-{arm64,x86_64}.log`,
`dynamic-start-stock-{source,bytecode}.log`,
`dynamic-start-x64-stock-{source,bytecode}.log`, and
`dynamic-start-ordinary-restoration.log`. Earlier failed diagnostics and the
first successful four-profile run are retained in that directory as well.

This is focused validation. The separate full gate remains frozen on its
preceding integration revision, and general x86_64 behavior parity is still
incomplete.
