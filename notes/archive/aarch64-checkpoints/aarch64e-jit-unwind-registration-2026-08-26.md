# macOS arm64e JIT unwind registration and landing authentication

## Outcome

The first arm64e failure in `lj_err_register_mcode()` was caused by the
assertion probe, not by a malformed registered FDE or personality pointer.
macOS 26's hardened arm64e libunwind cannot accept the public
`_Unwind_Find_FDE(const void *pc, ...)` input through its current
implementation: it routes the supplied `pc` through `unw_set_reg(UNW_REG_IP)`
and authenticates it as a key-B return address using the cursor's private stack
pointer. A caller can supply neither a raw address nor a correctly signed
ordinary function pointer with that discriminator.

Production now omits only this assertion on
`LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH`.
Registration and deregistration are unchanged. More importantly, both the
interpreter and JIT personalities now sign each `_Unwind_SetIP()` landing as a
return address with the exact cursor SP from `_Unwind_GetGR(ctx, UNW_REG_SP)`.
This is required by the same hardened setter and fixes a second PAC failure
that would otherwise occur during real cleanup/context installation.

## Failure reconstruction

The original assertion-build crash report was captured on macOS 26.5.1
(`25F80`) with Apple clang 21.0.0 (`clang-2100.1.1.101`). It reported
`EXC_ARM_PAC_FAIL` with this stack:

```text
libunwind::UnwindCursor<...Registers_arm64>::setReg + 96
unw_set_reg + 332
_Unwind_Find_FDE + 384
lj_err_register_mcode + 108
```

Live disassembly makes the ordering decisive:

- `_Unwind_Find_FDE + 380` calls `unw_set_reg` before its subsequent
  `get_proc_info` virtual call;
- `UnwindCursor::setReg + 84` loads the cursor SP;
- `setReg + 96` executes `autib` on the supplied IP; and
- the trap occurs there, before dynamic FDE lookup, CIE parsing, or personality
  decoding.

An isolated `-arch arm64e` probe called `_Unwind_Find_FDE` twice: once with a
normal signed C function pointer whose bits were preserved without a cast, and
once with the stripped/raw address. Both executions ended with SIGBUS status
138 at the same authentication instruction. A function-pointer cast therefore
cannot preserve the verifier.

This behavior is a regression in the lookup API's implementation, not evidence
that dynamic FDE registration itself is unsupported. Apple's published
libunwind source describes `_Unwind_Find_FDE` as constructing a cursor and
setting its IP, while the current LLVM implementation documents that changing
an arm64e cursor IP must authenticate the prior return-address form. See the
[Apple libunwind implementation](https://github.com/apple-oss-distributions/libunwind/blob/main/libunwind/src/UnwindLevel1-gcc-ext.c)
and [LLVM libunwind setter](https://github.com/llvm/llvm-project/blob/main/libunwind/src/libunwind.cpp).

## Registered personality audit

LuaJIT upstream commit
[`27af72e66f6a285298d1a9be370779aae945eb14`](https://github.com/LuaJIT/LuaJIT/commit/27af72e66f6a285298d1a9be370779aae945eb14)
added the ARM64e JIT personality signing used here. The current fork preserves
that ABI while supporting a writable alias:

1. The CIE uses `zPR`, an absolute personality pointer, and PC-relative
   signed-32 code encoding.
2. `err_unwind_jit` starts as an ordinary key-A function pointer.
3. `ptrauth_auth_and_resign` resigns it with the process-independent code key
   and the RX field address `info + ERR_FRAME_JIT_OFS_HANDLER`.
4. The signed bits are copied through `winfo`, but the discriminator remains
   the RX address from which libunwind reads the CIE.
5. LLVM's DWARF parser uses the encoded pointer's field address as the old
   arm64e discriminator before resigning it for its internal personality
   field. See [LLVM `DwarfParser.hpp`](https://github.com/llvm/llvm-project/blob/main/libunwind/src/DwarfParser.hpp).

The focused runtime test confirms this analysis: libunwind found the dynamic
FDE, parsed the CIE, authenticated the encoded personality, and invoked
`err_unwind_jit` in both search and cleanup phases. A malformed encoded pointer
would PAC-fail during CIE decoding instead of reaching those counters.

## Production changes

`src/lj_err.c` now has one macOS ARM64e helper shared by the interpreter and
JIT personalities:

```text
ptrauth_sign_unauthenticated(
  landing,
  ptrauth_key_return_address,
  _Unwind_GetGR(ctx, UNW_REG_SP))
```

`UNW_REG_SP` is named locally as DWARF/libunwind register `-2` because this
file deliberately carries private unwind declarations rather than depending
on inconsistent system unwind headers. The SP value is intentional: a CFA is
not generally interchangeable with the cursor register that libunwind uses to
authenticate `_Unwind_SetIP()`.

The `_Unwind_Find_FDE` assertion remains enabled on every target except the
exact `LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH` combination. Ordinary
ARM64, other Darwin ABIs, and non-Darwin PAUTH configurations are not broadly
suppressed.

## Executable contract

`tests/t-arm64e-jit-unwind.c` creates a real `MAP_JIT` page and asks
`lj_err_register_mcode()` to install the production JIT CIE/FDE at its front.
It writes a ten-instruction authenticated function behind the table:

```text
bti c
pacibsp
stp x29, x30, [sp, #-16]!
mov x29, sp
mov x16, x0
mov x0, x1
blraaz x16
brk #1
landing:
ldp x29, x30, [sp], #16
retab
```

The generated frame calls a normal signed C function which raises a real
LuaJIT exception with `lj_err_throw()`. A test-only branch inside the real JIT
personality replaces only the final `GCtrace` lookup/exit-stub selection. It
requires one search invocation, one cleanup invocation, and one installed
context, installs a known x0 result, and resumes at `landing`. The test then
deregisters the same FDE and unmaps the page.

Before that JIT traversal, the fixture also raises and catches a normal Lua
error through `lua_pcall()`. This exercises the interpreter personality's
authenticated `lj_vm_unwind_c_eh` installation through the same production
helper.

`tools/ci/arm64e_jit_unwind_contract.sh`:

- performs a clean real arm64e/BTI assertion build;
- compiles and runs the focused fixture as arm64e;
- proves the object still imports `__register_frame`, `__deregister_frame`,
  and `_Unwind_SetIP` but no longer calls `_Unwind_Find_FDE` on this ABI;
- pins the key-B/cursor-SP signing and the narrow assertion exclusion; and
- restores the ordinary ARM64 experimental build even when the contract fails.

Validated output:

```text
t-arm64e-jit-unwind OK: registered personality handled and landed
arm64e_jit_unwind_contract OK: interpreter and registered JIT personalities installed authenticated landings
```

## Remaining boundary

The test deliberately does not construct fake trace-table or snapshot state.
It proves dynamic registration, CIE/personality authentication, both unwind
phases, authenticated context installation, generated landing execution, and
deregistration. It does not yet prove a Lua error exiting a fully published
arm64e `GCtrace`; that requires the separate arm64e recorder/native-entry gates
and the currently constrained IR surface to be opened. Once those gates open,
an error-capable traced operation must reuse this contract as the lower-level
registration proof and add an actual `lj_trace_unwind()`/exit-stub run.
