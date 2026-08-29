# macOS ARM64e FFI callback pointer authentication (2026-08-26)

## Result

Generated LuaJIT FFI callbacks now cross the macOS ARM64e C function-pointer
ABI with a valid pointer-authentication code. A native `arm64e` executable built
with BTI created, called, changed, freed, and reused a generated callback
without a PAC or branch-target failure.

This change is independent of the experimental JIT recorder/entry gates. It
does not open or otherwise change any JIT surface.

## Boundary and fix

The callback page is published once and remains read-only. Each callback slot
inside that page is therefore a raw executable address. Before this change,
`callback_slot2ptr()` returned that raw address through FFI as if it were an
ordinary C function pointer. An indirect C call in an ARM64e process
authenticates a function pointer with the IA/function-pointer key and ABI
discriminator zero, so the raw slot address was not a valid exported value.

`src/lj_ccallback.c` now:

- signs a generated slot address with `lj_ptr_sign(p, 0)` only when
  `LJ_ABI_PAUTH` is enabled;
- strips that signature in `lj_ccallback_ptr2slot()` before doing address
  identity/range arithmetic; and
- performs the range calculation with integer addresses and rejects values
  below the callback page before subtraction, avoiding unrelated-pointer
  subtraction and underflow.

The callback slot allocator, owner claim/release order, function publication,
free tombstone, and page lifetime are unchanged. Ordinary ARM64 and x64 still
return the raw slot address because `callback_slot2ptr()` keeps its original
non-PAUTH path.

## BTI audit

The generated ARM64 callback slot already emitted `A64I_BTI_C` whenever
`LJ_ABI_BRANCH_TRACK` is enabled. The slot then loads its number and branches
directly to the shared callback-page head. The head loads the signed
`lj_vm_ffi_callback` function pointer and uses `A64I_BR_AUTH` (`BRAAZ` under
ARM64e). No generated-code change was necessary.

The contract deliberately combines both required build controls:

- compiler target flag `-mbranch-protection=bti`; and
- LuaJIT configuration flag `-DLUAJIT_ENABLE_CET_BR`.

That combination makes `LJ_ABI_BRANCH_TRACK=1`, selects the 12-byte ARM64
callback slot, and emits `BTI c` at every exported slot entry.

## Executable validation

`tools/ci/arm64e_ffi_callback_pauth_contract.sh` performs a clean
`-arch arm64e -mbranch-protection=bti` build and links
`tests/t-arm64e-ffi-callback-pauth.c`. The executable proves:

1. the exported pointer contains PAC bits and stripping it recovers the slot;
2. explicit IA/function-pointer authentication with discriminator zero works;
3. an actual indirect C call enters the Lua callback and returns the expected
   value;
4. the first generated instruction is the encoded `BTI c` instruction;
5. `callback:set()` changes the behavior observed through the same pointer;
6. free followed by allocation reuses the same lowest slot with the same valid
   signed pointer;
7. exact signed-pointer lookup returns the slot; and
8. signed interior, below-page, end-of-page, and unrelated function pointers
   are all rejected as unknown callbacks.

The contract also disassembles `lj_ccallback.o` and pins both the generated PAC
sign instruction and the signature-strip instruction. Its cleanup path restores
the shared checkout to an ordinary `-arch arm64` experimental build on success
or failure.

Observed output:

```
t-arm64e-ffi-callback-pauth OK: signed BTI callback create/call/set/free/reuse verified
arm64e_ffi_callback_pauth_contract OK: signed BTI callback create/call/set/free/reuse verified
```

After the restore build, the existing ordinary-ARM64 callback regressions also
passed with reduced stress counts:

```
t-ffi-callback-install OK: 2 threads, 32 callbacks
t-ffi-callback-runtime OK: 2 threads, 64 callback rounds
```

The two pre-existing build warnings remain: `szmcode` in `lj_trace.c` and
`ccall_rawchild_wait` in `lj_ccall.c` are unused in this configuration.

## Remaining scope

This proves the generated FFI callback pointer boundary on this macOS ARM64e
system. It does not by itself validate every FFI call lowering or reopen the
ARM64e JIT; those remain separate port gates.
