# Apple ARM64 frame-topology and result-range publication (2026-08-26)

## Claim boundary

This checkpoint makes the active JIT-disabled ARM64 VM publish complete Lua
call frames, return/result ranges, iterator call frames, vararg copies, and
vararg-function headers before their old stack topology stops being an
authoritative root source. It is the topology half of Stage 2, not the full
ARM64 root-publication gate. Ordinary leaf writes such as `ISTC`/`ISFC`,
`MOV`, `KSTR`, `KCDATA`, `UGET`, `FNEW`, `TNEW`, and `TDUP` remain in the next
slice, as do the legacy metatable/equality paths and ISNEXT bytecode
publication. ARM safepoint acknowledgement and the JIT remain disabled.

## Publication rule

The ARM implementation cannot inherit the x86-64 VM's plain-store/TSO
argument. Every slot or range changed by these paths is first written or
self-published with `stlr`; only after the complete replacement topology is
visible does the VM call `lj_state_stack_dirty_vm(L)`. Calls treat x0-x17 as
destroyed and retain live state only in the VM's x19-x28 callee-saved set.

`vm_call_publish` release-self-stores the function, frame PC, and complete
argument range for every interpreter call. This deliberately conservative
subroutine also covers frames assembled by tail calls and slow metamethod
paths. `ins_callt` then invalidates the TG stack-scan stamp and reloads the
callee from the published frame before dispatch.

Return paths keep the old roots authoritative until their replacements are
complete:

- protected returns release-publish their prepended true/false result;
- C returns invalidate the source-frame topology, release-copy results, dirty
  the completed range, and only then retire the source frame through
  `L->base`;
- the shared fast-function result path release-self-stores every actual result
  and release-fills any nil suffix before moving `BASE`;
- `RET`, `RETM`, `RET0`, and `RET1` release-copy/fill the caller result range
  and dirty it before installing the caller base.

`ITERC` release-publishes its callable/state/control triple. Both fixed and
copy-all `VARG` forms release-copy their destination range; the grow path
invalidates the current topology before stack relocation and reconstructs all
caller-clobbered addresses from bytecode. `IFUNCV` publishes its vararg header
before a possible grow, then copies each fixed argument to its destination
before release-clearing the old source slot. This destination-before-clear
order is the critical moving-root invariant for a concurrent low-to-high stack
scan.

An early inline `ins_callt` publication loop reused numeric DynASM labels at
each expansion site and produced a native SIGBUS in recursive vararg
tailcalls. Moving the loop into one internal subroutine removed that label
aliasing; the recursive tailcall/protected-call smoke then passed.

## Native validation

The final source passed a clean native build with:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src clean \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src -j8 \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
```

The emitted executable and VM object are native Mach-O ARM64. The object
contains the expected call/return/iterator/vararg symbols, 29 relocations to
`lj_state_stack_dirty_vm`, and no undefined compiler atomic-runtime helper.
The following runtime checks passed:

- vendored stock suite: 387/387;
- threading API, hook redispatch, and coroutine ownership/handoff;
- 4 x 80 concurrent FFI callback rounds;
- recursive vararg tailcall and protected-call smoke;
- the focused ARM64 result-retention fixture across full collections and GC
  worker pressure (`baseline=2`).

The focused fixture is useful semantic and retention stress for this batch,
but its dirty-delta attribution is not yet an independent proof for every
leaf opcode: the newly added CALL/RET/IFUNCV increments can make compound leaf
cases exceed the numeric baseline. The deterministic source/object contract
therefore intentionally remains red at the first deferred leaf family
(`ISTC`/`ISFC`) until those paths land and the fixture calibration is split.

## Remaining blockers before ARM safepoints

The next Stage 2 slice must complete the ordinary leaf stores listed above and
must make C-built metamethod frames release/root-published. The direct ARM
`getmetatable` path needs a generation-safe rooted helper; `setmetatable` can
use its existing C fallback. Distinct table/userdata equality must avoid naked
metatable/cache reads, and ISNEXT despecialization must publish target and
guard bytecodes through the bytecode helper protocol. Only after the complete
source/object/runtime gate is green may the VM acknowledge a TG root-scan
request.
