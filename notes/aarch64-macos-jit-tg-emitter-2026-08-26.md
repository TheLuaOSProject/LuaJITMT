# macOS ARM64 JIT TG-local emitter checkpoint

Date: 2026-08-26

## Outcome and safety boundary

This checkpoint replaces the experimental ARM64 assembler's compile-time
TG-emitter traps with real machine-code emission for the three shared
per-executor fields. The follow-up routing tranche also moves every ARM64
target-assembler access to `cur_L` or `jit_base` off the transitional
`global_State` mirrors and onto those primitives. It does not enable trace
recording, publication, linking, entry, exit or stitching.
`LJ_ARM64_JIT_FAIL_CLOSED` remains set for the experimental desktop build, and
the defensive gates in `lj_trace_hot()`, `lj_trace_ins()`, `trace_hotside()`
and `lj_trace_stitch()` remain unchanged. The runtime gate must therefore still
observe zero traces under forced-hot numeric loops.

## Callsite and width audit

The generic assembler has only these production ARM64 callsites:

| Function/path | Emitter | TG field | Width | Register contract |
| --- | --- | --- | --- | --- |
| `ra_rematk(REF_BASE)` | `emit_gettg` | `jit_base` | pointer/64-bit | allocator destination, never fixed x25 |
| `ra_rematk(ASMREF_L)` | `emit_gettg` | `cur_L` | pointer/64-bit | allocator destination, never fixed x25 |
| `asm_tail_link()` | `emit_settg` | `jit_base` | pointer/64-bit | source is exactly `RID_BASE`/x19, never x30 |
| `asm_head_root()` | `emit_setvmstate_root` | `vmstate` | signed 32-bit | positive trace number |
| `asm_head_side()` | `emit_setvmstate` | `vmstate` | signed 32-bit | positive trace number |

The apparent `emit_gettg(cur_L)` call in the generic call-argument collector is
inside `#if LJ_TARGET_X86ORX64` and is not an ARM64 callsite. The other
`emit_gettg`/`emit_settg` uses are in `lj_asm_x86.h` and do not widen the ARM64
contract.

The ARM64 target assembler adds exactly these five sites:

| Function/path | Emitter | TG field | Register and collision audit |
| --- | --- | --- | --- |
| `asm_retf()` | `emit_settg` | `jit_base` | `base` comes from `RSET_GPR`; it cannot be fixed x25 or scratch x30 |
| `asm_bufhdr_write()` | `emit_gettg` | `cur_L` | destination is x30; address formation followed by `LDAR x30,[x30]` is intentional |
| `asm_stack_check()` | `emit_gettg` | `jit_base` | destination is either x0 or selected from the supplied `RSET_GPR` allow-set, never x25/x30 |
| `asm_stack_check()` | `emit_gettg` | `cur_L` | destination is x30 with the same address-then-load contract |
| `asm_head_side_base()` | `emit_gettg` | `jit_base` | allocator destination `r` is drawn from `RSET_GPR`, excluding x25/x30 |

Reverse emission preserves the original runtime placement. In `asm_retf()`,
the BASE adjustment executes before the new address formation and release
store, so the published `jit_base` is still the adjusted lower-frame BASE. In
the load paths, each two-instruction ADD/LDAR block occupies the old load's
position relative to surrounding stack, buffer, guard and rematerialization
operations.

The only `emit_getgl`/`emit_setgl` uses left in `lj_asm_arm64.h` are the exact
global heap/collector allowlist: get/set `gc.grayagain` and loads of
`gc.threshold` and `gc.total`. No ARM64 target-assembler global access to
`cur_L`, `jit_base` or `vmstate` remains.

`TGState.cur_L` and `TGState.jit_base` are pointer-sized fields paired in C
with `la_loadptr_acq`/`la_storeptr_rel`. `TGState.vmstate` is exactly 32 bits
and is paired with `la_load32_acq`/`la_store32_rel`. On this layout, relative
to `TGState.dispatch`, the three offsets are 0x820, 0x830 and 0x84c. Static
assertions enforce pointer/word width, alignment and the immediate range used
by the emitter, so a future layout change fails compilation instead of silently
changing the addressing contract.

## Instruction and ordering contract

`RID_DISPATCH` is fixed x25 and remains the sole `TGState.dispatch` carrier.
`RID_TMP` is x30/LR, already excluded from register allocation, and is the sole
scratch used by these primitives.

Pointer loads emit, in execution order:

```text
add  x30, x25, #DISPATCH_TG(field)
ldar xD, [x30]
```

Pointer stores emit:

```text
add  x30, x25, #DISPATCH_TG(field)
stlr xS, [x30]
```

The load helper rejects x25 as a destination. The store helper rejects x30 as
a source, while the audited production source is x19. Thus address formation
cannot collide with a live allocator value and generated code never overwrites
the dispatch carrier.

Immediate `vmstate` publication needs an address and a value, but taking a
second allocatable register would clobber a potentially live side-trace value,
and temporarily adjusting x25 would invalidate the asynchronous carrier
contract. It therefore emits the release-equivalent sequence:

```text
movz/movn[/movk] w30, #exact-32-bit-state
dmb  ish
str  w30, [x25, #DISPATCH_TG(vmstate)]
```

`emit_loadi()` receives the state after an explicit `int32_t` conversion and
materializes its `uint32_t` bit pattern. Positive trace numbers and
bitwise-complemented negative states therefore have identical C and generated
code representations. `emit_setvmstate_root()` deliberately aliases the same
TG-local release publication, matching the non-Windows x64 semantic contract;
there is no global `vmstate` mirror.

x64 uses ordinary aligned MOV instructions relative to its fixed DISPATCH
carrier because x86 TSO supplies the acquire/release ordering required here.
ARM64 needs the explicit LDAR/STLR or barrier sequence above. Neither ARM64
primitive uses `RID_GL`, `J2G`, a `global_State` address or the transitional
global `jit_base`/`vmstate` mirrors.

## Focused validation contract

`tools/ci/arm64_jit_emitter_contract.sh` is run from the existing experimental
fail-closed gate and owns four independent checks:

1. It verifies that the live `lj_asm.o` is the member archived in
   `libluajit.a` and contains the test-only real-emitter wrapper.
2. It bounds the TG-emitter source region, requires the x25 acquire/release
   forms, and rejects every global-state addressing spelling. It also requires
   the exact five ARM64 target-assembler TG callsites, rejects any global
   `cur_L`/`jit_base`/`vmstate` emitter, and pins the four legitimate GC-global
   callsites.
3. `tests/t-arm64-jit-emitter.c` calls the production static emitters through
   the test-only wrapper and compares every generated word for `cur_L`,
   `jit_base`, positive/negative `emit_setvmstate` and positive/negative
   `emit_setvmstate_root`. It also asserts that x25/x30 are excluded from
   `RSET_GPR` while the production x19 BASE source remains allocatable.
4. It compiles `lj_asm.c` without inlining and inspects its Mach-O relocations,
   requiring all eight ARM64 `emit_gettg_` and three `emit_settg_` calls from
   the generic, target-specific and test-only inventories.
5. It wraps the exact generated bytes in a Mach-O `__TEXT,__text` section,
   disassembles the object, requires ADD/LDAR, ADD/STLR and MOV/DMB/STR forms,
   and rejects any x22/RID_GL reference.

The test helper exists only with `LJ_ARM64_EMIT_TEST_HELPERS`; it emits into a
caller buffer and never maps, publishes or executes the bytes.

## Validation evidence

Before the shared clean-build window, direct compile-only checks passed:

- thin native ARM64 assert compilation of `lj_asm.c`, including
  `_lj_asm_arm64_emit_test` in the object;
- thin native ARM64 assert compilation of the focused fixture; and
- thin x86_64 assert compilation of `lj_asm.c`, with no ARM64 test-helper
  symbol in the x64 object.

The standard no-JIT ARM64 bootstrap was then rebuilt and tested with these
source changes present. The full `tools/ci/arm64_bootstrap_gate.sh` run reported
387 passes across the threading, hooks, coroutine and FFI coverage and ended
with `arm64_bootstrap_gate OK`; the restored native artifacts reported
`jit.status() == false` and `jit.opt == nil`.

The final clean experimental command was:

```text
sh tools/ci/arm64_jit_fail_closed_gate.sh
```

It compiled and linked the JIT-enabled thin ARM64 runtime, passed the focused
emitter source/word/object contract, completed forced-hot numeric loops while
publishing no trace, passed the safepoint source contract and passed the remote
STOPREQ interpreter fixture. Its final line was:

```text
arm64_jit_fail_closed_gate OK: JIT linked, zero traces, interpreter safepoints sound
```

The focused contract was rerun against that exact archive. The relevant lines
from the Mach-O object made directly from the real emitter's output were:

```text
0000000000000000  add   x30, x25, #0x820
0000000000000004  ldar  x0, [x30]
0000000000000008  add   x30, x25, #0x830
000000000000000c  ldar  x1, [x30]
0000000000000010  add   x30, x25, #0x830
0000000000000014  stlr  x2, [x30]
000000000000001c  dmb   ish
0000000000000020  str   w30, [x25, #0x84c]
0000000000000028  dmb   ish
000000000000002c  str   w30, [x25, #0x84c]
0000000000000034  dmb   ish
0000000000000038  str   w30, [x25, #0x84c]
0000000000000040  dmb   ish
0000000000000044  str   w30, [x25, #0x84c]
```

The four DMB/STR pairs are respectively side-positive, side-negative,
root-positive and root-negative; the fixture's exact word assertions also
verify their preceding MOVZ/MOVN materialization. The disassembled bytes
contain no x22/RID_GL reference.

### ARM64 target-assembler routing follow-up

After migrating the five `lj_asm_arm64.h` callsites, both focused and full
experimental gates were rerun against freshly rebuilt artifacts:

```text
sh tools/ci/arm64_jit_emitter_contract.sh
sh tools/ci/arm64_jit_fail_closed_gate.sh
```

The focused gate passed the exact five-site TG/four-site GC-global source
inventory, then found eight `_emit_gettg_` and three `_emit_settg_` BR26
relocations in the non-inlined thin ARM64 assembler object. Its generated-word
fixture and disassembly remained green with the same x25-relative ADD/LDAR,
ADD/STLR and DMB/STR instruction lines recorded above.

The clean full gate rebuilt and linked the experimental JIT, repeated the
extended emitter contract, kept the trace count at zero under forced-hot
numeric loops, and passed the interpreter safepoint source and remote STOPREQ
runtime checks. It again ended with:

```text
arm64_jit_fail_closed_gate OK: JIT linked, zero traces, interpreter safepoints sound
```

Compile-only preservation checks with `-Wall -Wextra -Werror` also passed for
a thin ARM64 `LUAJIT_MT_ARM64_BOOTSTRAP + LUAJIT_DISABLE_JIT` object and a thin
default x86_64 assembler object. Neither preservation object contained the
ARM64 emitter test-helper symbol.

Integration evidence supplied by the parent for the preceding `ee5ffe9f`
checkpoint is also relevant to preservation: a thin x86_64 build with
`TARGET_FLAGS=-arch x86_64` passed the enabled-JIT numeric smoke and reported
509 stock-suite passes under Rosetta. `t-threading-api.lua -joff` panicked at
its caught closed-channel error on both `ee5ffe9f` and its parent `cc420988`,
so that failure is confirmed pre-existing and is not attributed to either
ARM64 checkpoint.

## Deferred runtime work

No part of the next runtime tranche is included. Exact TraceVec JLOOP/JFUNC
entry and retention, TG-local exit/stitch handling, weak-order `IR_XPOLL`, the
safe initial IR allowlist, trace publication/retirement and native mcode entry
all remain blockers before `LJ_ARM64_JIT_FAIL_CLOSED` can be removed.
