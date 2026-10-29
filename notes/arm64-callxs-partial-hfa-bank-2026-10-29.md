# ARM64 CALLXS partial-bank HFA spill

This checkpoint adds a bounded AAPCS64 case where the next HFA does not fit in
the remaining FP argument registers. AAPCS64 passes the entire HFA on the
stack and closes the FP argument bank for later scalar FP arguments. The
previous classifier rejected such layouts whenever any FP register remained.

The implementation derives the fixed CALLXS signature for the specific
guarded CData function and uses that ABI plan for register setup and outgoing
stack reservation. The emitter places every member of a stack HFA on the
stack, then places later FP arguments there too. Semantic and post-RA admission
independently check the grouped members, source type and identity, stack
offsets, register-bank exhaustion, and carrier widths. Reservation uses the
classifier's complete stack extent, including following arguments. Unsupported
or variable-arity signatures retain the prior conservative fallback/refusal.

The native parity witness uses seven scalar double arguments, a two-float
HFA, then a trailing double. ARM64 must spill the complete HFA despite V7
remaining, and spill the trailing double after it. The ordinary x86_64 peer
passes the 8-byte HFA in XMM7 and places the trailing scalar on the stack. The
test checks interpreter/native equality, native trace completion, child
exits, callable replacement rejection, and GC/re-entry. Independent semantic
and post-RA lane-swap mutations reject the malformed HFA map.

The classifier also checks a four-float HFA with a partially available bank;
its full 16-byte group is accepted by the classifier's bounded stack layout,
but this does not claim a cross-target native witness for that shape. A
three-float (12-byte) HFA remains refused in the partial-bank case because the
current generic spill lowering only supports groups with an eight-byte-aligned
complete stack extent. Variable-arity calls and other aggregate shapes are
outside this change.

Validation on the frozen source:

- `tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed profiles
  `arm64-assert`, `arm64e-assert`, `arm64-release`, and `arm64e-release`.
  Captured log: `/Users/frityet/Scratch/ljmt-hfa-partial-bank-20261029/final-arm64-four-profiles.log`.
- The ordinary x86_64 peer completed its JIT loop with matching oracle output
  and 320 native calls. Its trace contains the expected seven scalar inputs,
  one aggregate carrier, and trailing double at `CALLXS`.
  Captured log: `/Users/frityet/Scratch/ljmt-hfa-partial-bank-20261029/final-x64-native-peer.log`.
- The x86_64 executable and aggregate library were built from this source
  checkpoint; `src/luajit` reported architecture `x86_64`.
