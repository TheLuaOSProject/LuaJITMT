# Apple ARM64 leaf stack publication (2026-08-26)

## Claim boundary

This checkpoint completes the ordinary interpreter leaf writes required by
the deterministic Stage 2 root-publication contract. The active JIT-disabled
ARM64 VM now release-publishes the result of `ISTC`/`ISFC`, `MOV`, `KSTR`,
`KCDATA`, `UGET`, `FNEW`, `TNEW`, and `TDUP`, and invalidates the TG stack-scan
stamp only on paths that actually changed a stack slot.

This is not the end of Stage 2. C-built metamethod frames, generation-safe
metatable and equality handling, and `ISNEXT` bytecode publication remain
open. ARM safepoint acknowledgement and the ARM JIT therefore remain
disabled.

## Publication rules

Simple copy and constant bytecodes calculate the destination address, store
the complete `TValue` with `stlr`, and then invoke
`lj_state_stack_dirty_vm(L)`. `ISTC` and `ISFC` branch around both operations
on their non-copy fallthrough, so a read-only test cannot manufacture a false
dirty epoch.

Allocator calls are allowed to relocate the Lua stack and clobber temporary
registers. `FNEW`, `TNEW`, and `TDUP` therefore reload `L->base` and re-decode
the RA byte from the current instruction after returning from C. Fresh tables
are already published by their constructors, so their tagged stack root uses
a release store followed by the VM dirty helper. A fresh closure additionally
calls `lj_state_stack_pubtv(L, L, dst)`, which supplies the collectable-root
publication/barrier protocol and performs the dirty transition itself.

`UGET` retains the existing acquire load from the upvalue cell and now
release-publishes that exact snapshot into the destination slot before
dirtying. This preserves the upvalue's value-generation pairing rather than
re-reading it after publication.

## Native validation

The integrated ARM64 bootstrap gate performed a clean native assert build
with `LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_DISABLE_JIT`, and `LUA_USE_ASSERT`.
It then passed:

- the TG-local dispatch/static atomic contract;
- the deterministic source and emitted-object root-publication contract,
  including direct `BC_UGET` code and relocation checks;
- the focused root-retention fixture with table, number, and nil cases across
  full collections (`baseline=3`);
- all 387 vendored stock tests;
- threading API, hook redispatch, and coroutine handoff/dead-resume checks;
- four concurrent FFI callback workers for 320 callback rounds.

The runtime fixture proves bytecode reachability, semantics, and retained
collectable results under collection pressure. It deliberately does not claim
per-opcode dirty-delta attribution: the already-published call/return envelope
also advances the dirty epoch. Exact opcode ownership is instead enforced by
the source/object contract, which rejects a missing release store, missing or
misplaced dirty transition, stale generated object, or missing VM relocation.

## Remaining Stage 2 work

Before ARM can acknowledge a TG root-scan request, the port still needs:

- release/root publication for the frames assembled in `mmcall`,
  `lj_meta_cat`, and `lj_meta_call`;
- a generation-safe rooted protected-metatable helper for ARM
  `getmetatable`, with `setmetatable` routed through its C implementation;
- rooted distinct table/userdata equality and comparison handling;
- helper-mediated publication of both bytecodes changed by `ISNEXT`.

Only the completed metatable/equality/bytecode gate can close Stage 2 and make
safepoint acknowledgement sound.
