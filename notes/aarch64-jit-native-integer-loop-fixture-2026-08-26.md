# macOS ARM64 native integer-loop execution fixture

## Scope

`tests/t-arm64-jit-native-loop.c` is the first end-to-end execution proof for
the experimental macOS ARM64 JIT. It deliberately admits only the exact
integer `BC_LOOP` root produced by:

```lua
local function f(n)
  local i, x = 0, 0
  while i < n do
    i = i + 1
    x = x + i
  end
  return x
end
```

The fixture is not a general ARM64 JIT smoke test. For both ordinary ARM64 and
ARM64e/BTI, it pins all six granular gates to the first-loop policy: root recording
and loop entry are open; side and stitch recording plus `JFUNCF` and stitch
entry remain closed. The older recorder-admission and native-entry macros are
compatibility summaries and are not used as behavioral predicates.

## Runtime proof

The fixture starts with a flushed JIT and `hotloop=1`, `hotexit=1`, and
`maxtrace=2`. It calls the Lua function five separate times from C. Each call
must return exactly `210`, and the saved `L->cframe` must be identical after
every call.

The entry/exit test hooks then require:

- five direct root-entry publications and zero rejection cleanups;
- five native trace exits, all through root trace 1 and final snapshot 8;
- restored TG VM state and a cleared `jit_base`;
- one runnable root trace, self-linked as `LJ_TRLINK_LOOP`, with no children,
  no side-chain link, and no other runnable trace;
- the exact root prototype, original `BC_LOOP`, patched `BC_JLOOP 1`, forward
  loop extent, backward `BC_JMP`, prototype frame extent, and
  `TRACE_ARM64_INT_LOOP_ADMITTED` publication bit;
- non-null aligned machine code, an exact 168-byte body plus one `BTI J` word
  when branch tracking is enabled, an aligned loop offset strictly inside the
  body, and zero `spadjust`;
- no allocated spill slot in any final IR instruction.

The semantic IR contract has one integer constant (`+1`) plus the canonical
three primitive constants. After `IR_BASE`, its exact instruction sequence is
two guarded integer loads, two guarded overflow additions, the bound load and
precondition, `LOOP`, `XPOLL`, two loop-body additions, the loop condition, and
two integer `PHI`s. Register allocation may append only these two records:

```text
RENAME R_I_NEXT, 5
RENAME R_X_NEXT, 5
```

The nine snapshot references are pinned as well, so the final allocator view
cannot conceal a semantic IR or exit-topology change.

## VM stack contract

`tools/ci/arm64_jit_native_loop_contract.sh` performs a clean experimental
build, compiles and runs the fixture, and checks both source and generated VM
code. In the successful `BC_JLOOP` arm, `sub sp, sp, #16` must occur before
`br_trace_auth CARG2, CRET1`. The ordinary ARM64 disassembly must likewise put
`sub sp, sp, #0x10` before `br x1`; ARM64e must put it before
`braa x1, x0`. This restores the fixed interpreter spill area expected by the
native exit path; pointer authentication changes the branch instruction, not
the ordering requirement.

## ARM64e execution checkpoint

The same exact root now executes end to end in an assertion-enabled
`-arch arm64e -mbranch-protection=bti` build. Production opens root recording
and `BC_LOOP` native entry for both ARM64 ABIs. Side and stitch recording,
`JFUNCF` entry, and stitch entry remain independently fail-closed.

Two earlier native arm64e probes exposed independent failures before trace
publication:

- the assertion build first fails inside `_Unwind_Find_FDE` while
  `lj_err_register_mcode()` verifies the newly registered JIT unwind table;
- after removing only that assertion from the diagnostic build, the release
  probe fails at `lj_asm_trace+468`: an optimized raw address for the
  `lj_vm_exit_handler` byte-array label reaches `autiza` as though it were a
  signed function pointer.

The target-materialization failure is now fixed without changing ordinary
ARM64. Raw VM assembler labels use `emit_asmlabel_addr()` and never enter a
pointer-authentication operation. Genuine runtime `ASMFunction` values retain
their signed representation through `ptrauth_nop_cast()` and are then
explicitly authenticated for address arithmetic. The out-of-range indirect
call path continues to embed the original signed value for `BLRAAZ`.

`tools/ci/arm64_pauth_emit_target_contract.sh` builds and executes the focused
proof once as ordinary ARM64 and once as a real ARM64e/BTI executable (using
`-arch arm64e -mbranch-protection=bti`). Both runs print:

```text
t-arm64-pauth-emit-target OK
```

The ordinary ARM64 wrappers contain no `AUT`, `PAC`, or `XPAC` instruction.
In the ARM64e object, direct `lj_vm_exit_handler` materialization is an
`adrp`/`add` sequence with no pointer-authentication instruction, while a
runtime-loaded signed function pointer executes `autiza` followed by the
compiler's `xpaci` identity check. The indirect-bit wrapper contains no
authentication or stripping operation, and its executable assertion proves
that the signed bits survive unchanged. Relocations at the real
`lj_vm_exit_handler` and `lj_vm_exit_interp` assembler callsites are raw
`PAGE21`/`PAGOF12` pairs.

The unwind failure is handled separately by
`tools/ci/arm64e_jit_unwind_contract.sh`. That executable proof registers a
real JIT FDE, reaches its personality in search and cleanup phases, installs
an authenticated landing, executes it, and deregisters the FDE. It also
proves the interpreter landing. The hardened macOS ARM64e
`_Unwind_Find_FDE` assertion is excluded only on this ABI because that API
authenticates its caller-supplied PC with an unavailable cursor-SP
discriminator; production registration and deregistration are unchanged.

The native-loop fixture pins the ARM64e differences rather than treating them
as ordinary ARM64:

- branch tracking adds one leading `BTI J` word, so the exact body is 172
  bytes instead of 168;
- the VM reserves the same 16-byte fixed spill area and then enters with
  `BRAA x1, x0`, authenticating the trace pointer with its `GCtrace` address;
- normal mcode placement executes all five exits through the direct `BL`
  exit-handler stub;
- a second process, built with `LUAJIT_MCODE_TEST` and run with
  `LUAJIT_MCODE_TEST=R`, forces the real trace out of direct range, asserts the
  K64 handler load plus `BLRAAZ` encoding, and executes the same five entries,
  results, and exits; and
- a final `jit.flush()` restores the original `BC_LOOP`, clears the prototype
  root, makes trace slot 1 non-runnable, and leaves the TG-local JIT base clear.

This remains a deliberately narrow integer-loop result. It does not open or
claim side traces, function/stitch entry, spills, general scalar IR, or an
error-capable published trace. The focused unwind contract proves the lower
level FDE/personality path, but this loop has no throwing operation with which
to exercise `lj_trace_unwind()` from a published `GCtrace`.

## Validation performed

The complete contract passed natively on Apple ARM64 for ordinary ARM64 and
for real ARM64e/BTI direct and forced-far exit-handler placements:

```text
t-arm64-jit-native-loop OK
t-arm64-jit-native-loop OK          # ARM64e direct exit-handler BL
t-arm64-jit-native-loop OK          # ARM64e K64 plus BLRAAZ forced far path
arm64_jit_native_loop_contract OK: strict ARM64 and ARM64e/BTI BC_LOOP executed direct and authenticated far exits
```

After the production root-helper changes landed in the working integration
tree, the strengthened fixture was also compiled from this isolated worktree
against those headers and archive and passed natively. That run includes the
per-call `cframe`, admission-bit, prototype-geometry, and machine-code
alignment assertions. The contract also executes a second one-PHI loop and
requires exactly one final `IR_RENAME`, covering the semantic-plus-one case
without confusing it with the allocator's spare NOP.

The focused pointer-authentication checkpoint also passed natively:

```text
t-arm64-pauth-emit-target OK          # ordinary ARM64
t-arm64-pauth-emit-target OK          # ARM64e plus BTI
arm64_pauth_emit_target_contract OK: direct and signed runtime targets normalize on ARM64e
```

The focused dynamic-unwind proof also passed in an ARM64e/BTI assertion build:

```text
t-arm64e-jit-unwind OK: registered personality handled and landed
arm64e_jit_unwind_contract OK: interpreter and registered JIT personalities installed authenticated landings
```
