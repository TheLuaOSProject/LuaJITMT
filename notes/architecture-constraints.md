# Durable lockless architecture constraints

These constraints survive individual implementation tranches. They are the
review boundary for both x86-64 and Apple ARM64 work.

## Locklessness and ownership

- Preserve the fork's fully lockless target for the VM, GC2, FFI, callbacks,
  recorder, assembler, trace lifecycle, and generated code. The named policy
  exceptions such as `ffi.cdef` and `require` do not authorize hidden locks in
  unrelated paths.
- Do not replace a removed global lock with an atomic helper that may call a
  lock-based runtime. Exact shared authorities must remain genuinely
  lock-free on every enabled target.
- Keep dispatch, hotcounts, VM state, native-frame state, temporary state,
  recorder ownership, and safepoint requests TG-local unless a protocol
  explicitly publishes shared immutable state.
- Generated `IR_TMPREF` storage belongs to the executing TG. `IN1`/`OUT1`
  address `tmptv`; `IN2`/`OUT2` address `tmptv2`. Never alias the two slots or
  regress generated code to universe-global scratch storage.

## Publication and weak ordering

- Preserve exact 128-bit authorities for GC2 activation, arena markwords,
  universe admission, TG lifecycle, table descriptors, and arena registry
  entries. Assert alignment and prove inline lock-free lowering for the target.
- Use explicit acquire/release publication on ARM64. An x86 TSO or plain-MOV
  argument is never sufficient evidence for Apple Silicon.
- Publish complete `TValue` objects without tearing. Shared table readers and
  writers must validate forwarding, generation, ownership, and retirement.
- A diagnostic or approximate split snapshot must never be reused where an
  exact recurring-state authority is required.

## JIT and executable memory

- Treat traces as immutable after publication. Validate slot, incarnation,
  parent, entry generation, and liveness before entering or linking.
- Retain trace bodies and machine code for every possible reader; use exit
  indirection and safe retirement rather than patching freed targets.
- Keep macOS executable-memory writes compatible with `MAP_JIT`, hardened
  runtime rules, instruction-cache synchronization, PAC, and BTI.
- New ARM64 IR shapes stay fail-closed until source bytecode, semantic IR,
  post-register-allocation layout, snapshots, emitted instructions, exits,
  lifecycle, and adjacent negative shapes are independently certified.
- Do not centralize independent semantic and post-RA certificates merely to
  reduce line count; that would create a common-mode acceptance bug.
- Physical co-location in a target-local header is fine, but semantic and
  post-RA admission must remain separate passes with independent negative
  fixtures and must not share one acceptance result.
- Opening one exact ABI certificate does not authorize adjacent ARM64 CALLXS
  signatures, indirect callees, or call-site/root geometries.

## Native calls, callbacks, and signals

- Publish exact native call/callback frames and all stack/result roots. Preserve
  nested callback, auto-attach/detach, error, unwind, and post-call cleanup.
- The cdata base metatable and its `__call` entry are mutable. Sample and execute
  through rooted helpers, guard both the current base-metatable root and exact
  function identity, and never retain a raw table slot across concurrent
  mutation or resize.
- Darwin AAPCS64 register classes, indirect aggregate results, variadics,
  errno, PAC, BTI, and unwind metadata require positive target evidence.
- Restore `errno` or `LastError` at the final target landing after context
  installation; a personality return alone cannot protect it from unwinder
  clobber.
- Signal handlers must not allocate, enter the dynamic loader, perform unsafe
  TLV lookup, wait on the scheduler, or touch state without an async-signal-safe
  publication contract.

## Minimal divergence from LuaJIT

- Keep upstream control flow, naming, layout, and formatting where the
  lockless protocol does not require a change.
- Prefer target-local helpers and narrow architecture guards over unrelated
  edits to common LuaJIT paths.
- Avoid cosmetic churn, speculative abstractions, and duplicated compatibility
  layers. Preserve a small diff only when it does not weaken independent safety
  gates or hot-path performance.
- Put exhaustive proof machinery in tests and CI contracts. Production code
  should contain the smallest readable gate and runtime mechanism that those
  contracts can certify.
- Separate implemented, live-verified behavior from designs, dry runs, and
  historical checkpoints.
