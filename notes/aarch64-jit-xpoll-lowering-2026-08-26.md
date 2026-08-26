# ARM64 JIT XPOLL lowering checkpoint (2026-08-26)

This checkpoint implements the missing ARM64 backend lowering for `IR_XPOLL`
while the experimental recorder and native-entry paths remain fail-closed. It
does not claim that an ARM64 trace has executed. The recorder and loop
optimizer still insert `IR_XPOLL` only for x64; ARM64 insertion belongs to the
later native-execution checkpoint. This lowering is the required backend half
of preventing a pure compiled loop from ignoring a GC2 gate close, a
safepoint request, or a profiler request indefinitely.

## Runtime order

Every ARM64 XPOLL first acquire-loads the global 32 bit
`gc2.jit_phase_gate`. A zero value exits through the instruction's ordinary
snapshot guard. When the IR literal requests an executor poll, generated code
then acquire-loads `TGState.poll` and `TGState.profile_request` separately,
ORs the two 32 bit values, and exits through the same snapshot when either is
nonzero.

The order is intentional:

```text
LDAR  gate
CMP   gate, 0
BEQ   snapshot exit
LDAR  tg.poll
LDAR  tg.profile_request
ORR/CMP
BNE   snapshot exit
```

The TG fields are distinct natural-width publications on ARM64. The x64
backend's overlapping qword comparison is not valid here. `x25` remains the
fixed `TGState.dispatch` carrier, and reserved `x30` is the sole address
scratch. The global GC2 offset is formed with a layout-derived one- or two-ADD
sequence before the acquire load, so later layout growth does not silently
change the ordering or width contract.

## Validation

The existing ARM64 emitter fixture now checks exact instruction words for
32 bit acquire loads of `poll`, `profile_request`, and the JIT phase gate, and
the companion contract disassembles those words as a Mach-O ARM64 text
section. The contract also verifies the reverse-emitter ordering in
`asm_xpoll()`, rejects 64 bit/combined TG loads, and proves the generic
assembler no longer maps ARM64 XPOLL to its no-op fallback.

These checks are included by the experimental fail-closed gate. A later
recorder-enable checkpoint must still prove a real snapshot-bearing trace
exits for each of: a closed phase gate, a TG poll request, and a profile
request. Native entry must remain disabled until the TG-local exit path clears
`jit_base`, publishes `INTERP`, and acknowledges those requests after snapshot
restoration.
