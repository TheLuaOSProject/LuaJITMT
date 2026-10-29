# ARM64 direct aggregate-result width audit

This audit compared the ordinary ARM64 runtime built at the existing
`c3c77156a` checkpoint with the ordinary x86_64 runtime from
`dae24b46aad9d4fc1d36cdce5b94d49d05d4743a`. No production code or tests in
the repository were changed for the probes.

## Direct 9–16 byte results

The probe calls packed result structs of 9, 10, 12, and 15 bytes, plus a
naturally aligned 16-byte `{uint64_t, uint64_t}` result. On each architecture,
all five functions returned the expected cdata values and accumulated 560 C
effects. The runner had no published root traces, hence no selected native
`CALLXS` root or selected-root exit on either architecture.

This is a shared recorder refusal, not evidence for an x64-only parity gap.
On x64, `crec_call_sysv_aggregate_type()` rejects sizes above 8 bytes, while
`crec_call_indirect_struct_result()` enables the x64 sret path only above 16
bytes (or for non-register sizes on Win64). Thus the 9–16 byte interval has no
recorded native return path in this implementation. The interpreter behavior
was checked, but these cases do not justify widening ARM64 admission.

## Confirmed x64-only case: pointer-only 8-byte result

The second probe returns `struct { void *p; }`. The ARM64 ordinary runtime
matched the interpreted oracle and performed 640 C effects, but published no
runner root. The x86_64 runtime published selected root 1 containing the
native call (`nins=57`), observed 47 selected-root exits, and performed 720 C
effects. A live peer remained attached during each run. On x64, the selected
root's machine-code pointer, address, and loop entry remained identical after
full GC and an alternate callee; the alternate result matched the independent
JIT-disabled oracle and caused another selected-root exit.

The source explains the difference. The SysV x64 classifier places a pointer
field into the INTEGER class and the 8-byte aggregate becomes a U64 carrier.
The ARM64 small-result leaf walker only permits integer/enum/float scalar
leaves; its pointer scalar kind is deliberately rejected. This is a concrete
small-result parity candidate if prioritized later. It must preserve pointer
identity and the result-box/root lifetime, rather than treating the payload as
an arbitrary integer.

## Reproduction artifacts

All files are under `/Users/frityet/Scratch/sret-profile-tmp/`:

- `direct-result-probe.{lua,c}` and thin `direct-result-{arm64,x86_64}.dylib`
- `direct-result-{arm64,x86_64}.log`
- `pointer-result-probe.{lua,c}` and thin `pointer-result-{arm64,x86_64}.dylib`
- `pointer-result-{arm64,x86_64}.log`

The ARM64 runtime SHA-256 is `680255525d48defca133822873fda43926ee2326a3c1b31d98815998f422fd39`;
the x86_64 runtime SHA-256 is
`5998c9d47d84d717619d0ef50e339550c9cbe361fd2cbf3fcf8a6fadc4cfe04c`.
The exact probe and helper hashes are preserved in the task log alongside
these artifacts. The source checkout remains at `c3c77156a`.
