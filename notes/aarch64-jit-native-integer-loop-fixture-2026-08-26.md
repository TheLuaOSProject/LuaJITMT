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

The fixture is not a general ARM64 JIT smoke test. For an ordinary ARM64
slice, it pins all six granular gates to the first-loop policy: root recording
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
- non-null aligned machine code, an exact 168-byte body, an aligned loop
  offset strictly inside the body, and zero `spadjust`;
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
`sub sp, sp, #0x10` before `br x1`. This restores the fixed interpreter spill
area expected by the native exit path; pointer authentication changes the
branch instruction, not the ordering requirement.

## Current arm64e boundary

The end-to-end result in this note is for an ordinary `-arch arm64` binary.
The arm64e/BTI contracts compile the same sources and verify the emitted
authenticated substrate, but they do not yet execute generated trace code
successfully. Production now enforces that boundary: when `LJ_ABI_PAUTH` is
true, both root recording and direct loop entry remain fail-closed. The
arm64e contract runs the interpreter with JIT enabled and requires that no
trace is published.

Two native arm64e probes fail before trace publication with
`EXC_ARM_PAC_FAIL`:

- the assertion build first fails inside `_Unwind_Find_FDE` while
  `lj_err_register_mcode()` verifies the newly registered JIT unwind table;
- after removing only that assertion from the diagnostic build, assembly
  fails at `emit_asmfunc_addr()` when an optimized raw address for
  `lj_vm_exit_handler` reaches `autiza` as though it were a signed function
  pointer.

The second failure proves that suppressing the unwind assertion is not a
solution. A later arm64e tranche must fix direct runtime-symbol materialization
and then return to JIT unwind registration and exception-through-trace tests
before this fixture can claim an authenticated end-to-end execution result.

## Validation performed

The complete contract passed natively on Apple ARM64 against a temporary
integration overlay while the granular entry work was in flight:

```text
t-arm64-jit-native-loop OK
arm64_jit_native_loop_contract OK: exact integer BC_LOOP recorded, assembled, published, entered and exited natively
```

After the production root-helper changes landed in the working integration
tree, the strengthened fixture was also compiled from this isolated worktree
against those headers and archive and passed natively. That run includes the
per-call `cframe`, admission-bit, prototype-geometry, and machine-code
alignment assertions. The contract also executes a second one-PHI loop and
requires exactly one final `IR_RENAME`, covering the semantic-plus-one case
without confusing it with the allocator's spare NOP.
