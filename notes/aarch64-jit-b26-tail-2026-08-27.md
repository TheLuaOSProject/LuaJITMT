# ARM64 tail B26 hardening (2026-08-27)

## Scope

This checkpoint hardens only the final ARM64 trace-tail `B` instruction. It
does not open side recording, consume either first-side certificate, or widen
the admitted IR/branch surface.

The old tail code subtracted an executable support-code pointer from a JIT
mcode pointer. Those pointers do not belong to one C object. It also treated
every linked target as encodable, so an out-of-range displacement could be
silently truncated to 26 bits.

## Address contract

`lj_asm_arm64_b26_encode()` is a public, side-effect-free address encoder. It
accepts `uintptr_t` source and target addresses and writes one host-order
`A64I_B` instruction only when:

- source, target, and output are non-null;
- both addresses are four-byte aligned;
- the ordered unsigned difference is within the exact signed B26 byte range:
  `-0x08000000` through `+0x07fffffc`.

It performs no cross-object pointer subtraction and leaves the output
unchanged on rejection. The word stays in host order because the normal ARM64
mcode finalizer performs the eventual ARM64BE conversion.

## Tail behavior

`asm_tail_fixup()` now uses the encoder for both interpreter and linked
targets. A rejected linked target raises `LJ_TRERR_MCODEOV`; it can no longer
be truncated. A rejected interpreter target retains the existing two-word
K64 load plus authenticated indirect branch.

`asm_tail_prep()` probes both possible direct-branch PCs before selecting the
one-word interpreter tail: the initial `p-1` when no SP adjustment is emitted
and the initial `p` when it is. If either placement is not encodable, it
reserves the extra word needed by the indirect fallback.

## Verification

`tests/t-arm64-jit-b26.c` covers both exact limits, the first rejected value on
each side, every aligned displacement in a 16-byte window around both limits,
null/unaligned/wrap-edge addresses, unchanged reject output, round-trip decode,
and all 32 possible single-bit mutations of each accepted instruction.

`tools/ci/arm64_jit_b26_contract.sh` adds source-shape checks for unsigned
address arithmetic, host-order staging, correct `p-1`/`p` reservation, linked
`MCODEOV`, and the interpreter fallback. It compiles the implementation and
fixture for arm64e, runs the fixture as native arm64, and is part of the full
fail-closed ARM64 umbrella.

This is an enabling safety checkpoint, not evidence that first-level ARM64
side traces can yet be recorded, linked, published, or executed.
