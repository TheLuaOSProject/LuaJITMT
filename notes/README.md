# LuaJITMT ARM64 port status

This directory is intentionally small. It contains the current ARM64 status
and the durable lockless architecture constraints. The 938 historical notes
and reports that are no longer authoritative are retained in
[`archive/`](archive/README.md).

## Status at a glance

- Development branch: `codex/aarch64-macos-port`.
- Upstream comparison point: LuaJITMT `v2.1` at `a649f737`.
- ARM64 remains opt-in with `LUAJIT_MT_ARM64_BOOTSTRAP`.
- Native ARM64 JIT work additionally requires
  `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`.
- Latest source-fidelity cleanup: `edbf57fa`.
- Latest ARM64 VM acquire-load cleanup: `3f109fa2`.
- Latest ARM64 interpreter GC2 parity: `d639196b`.
- Latest ARM64 JIT GC2 cadence parity: `f1fe1f73`.
- Latest complete fail-closed gate: `f1fe1f73`.
- After validation, the checked-out build was manually restored to the safe
  ARM64 profile, with `jit.status() == false`.

This is a working Apple Silicon port with substantial native execution
evidence. It is not yet a completed or general ARM64 LuaJIT backend. Native
JIT recording and entry outside the exact admitted shapes below are
intentionally rejected; ordinary interpreted Lua remains available.

## Implemented and exercised

### Interpreter and lockless runtime

- Lockless interpreter bootstrap, TG-local state, safepoints, frame and root
  publication, protected calls, metamethods, selected table operations, and
  interpreter FFI/callback lifecycle.
- Explicit ARM64 acquire/release ordering for the audited mutable VM roots and
  edges. The target-local VM acquire primitive now covers C-closure upvalues,
  base-metatable roots, Lua 5.2 iterator metatables, Lua-closure `uvptr[]`,
  `L->openupval`, and `GL->wrapf`.
- Forwarding-safe `lj_vm_next` array/hash traversal on ARM64 and arm64e/BTI,
  including active mutation, generation forwarding, retirement retry,
  separated arrays, hidden keys, and caller-owned result snapshots.
- Callback-result lifetime through post-detach TG reclamation.
- GC2 hard-assist parity at interpreter allocation checks, including the fast
  function helper and the `TNEW`/`TDUP` bytecode paths. Shared pacing fields
  are acquired explicitly and the exact local-debt batch boundary is covered.

### Bounded native JIT

- ARM64 and arm64e/BTI trace publication, authenticated entry and exits,
  retirement, flush/reuse, GDB-JIT preparation, and bounded root/first-side
  entry paths.
- Exact integer and numeric loop certificates, including constant-step integer
  `FORL`, one spill-free variable-stop/variable-step integer `FORL`, bounded
  integer and mixed numeric `LOOP` roots, and their independent semantic,
  snapshot, and post-register-allocation checks.
- A bounded set of first-side traces. General side traces, side-of-side traces,
  and stitches remain closed.
- TG-local `XSAVE` staging and native-call frame lifecycle for two exact Darwin
  ARM64 root geometries and three exact CDECL signatures:
  `int32_t(int32_t)`, `double(double)`, and
  `const char *(const char *)`.
- Exact FFI lifecycle evidence covers errno, normal and forced exits, STOPREQ,
  one-level and depth-two callbacks, callback errors, cleanup/reuse, remote GC
  and flush, mutable cdata `__call`, double ABI/results, boxed-pointer identity,
  and result-root survival.
- Lockless OS-error restoration for C, fast-function, and the certified JIT
  unwind path. The standalone arbitrary arm64e JIT-frame contract is still
  synthetic; only the exact integer `CALLXS` STOPREQ path has real trace-unwind
  evidence.
- Trace-head GC checks acquire both color-GC fields and both GC2 cadence fields,
  matching x64's `total >= threshold || alloc_since > hard_check` behavior.
  The admitted boxed-pointer `CALLXS` trace proves both equality-skip and
  strict-overdue paths without flushing sub-batch debt or starting stopped GC.

## Deliberately closed or incomplete

- General ARM64 JIT IR admission for arbitrary Lua programs, unrestricted
  spill or register layouts, and general root geometries.
- General table and upvalue JIT reads, writes, traversal, allocation, and other
  generated heap effects. Forwarding-safe interpreter `lj_vm_next` does not by
  itself open these JIT paths.
- General side traces, side-of-side traces, stitches, and production JIT
  side-exit unwind resolution.
- Generic Darwin ARM64 `CALLXS` lowering. Other signatures, indirect callees,
  variadics, aggregates/sret, other pointer types, and other call-site
  geometries remain rejected.
- Full parity with every x86_64 lockless VM, JIT, FFI, profiler, unwind, and
  stress path.
- Complete sanitizer, weak-memory, sustained-concurrency, and performance
  recovery matrices.
- Pre-activation legacy closures with still-open upvalues do not yet have an
  explicit sharing policy or lifetime proof across `threading.spawn`/attach.
  Current tests cover captures created after activation, not this older edge.

## Minimal-divergence review

Across the earlier and current minimal-divergence checkpoints, touched code was
compared with the `v2.1` control flow and style. The reviews removed only
divergence that was not part of the lockless ARM64 contract:

- restored unchanged build options and non-ARM behavior where earlier work had
  widened a target guard;
- kept the large ARM64 admission policy in `src/lj_asm_arm64_admit.h` instead
  of growing shared assembler control flow;
- kept semantic and post-register-allocation certificates independent even
  where merging them would reduce line count;
- hid test-stage constants behind `LJ_TRACE_TEST_HELPERS` and removed a
  redundant exported side-validator test wrapper;
- normalized DynASM indentation, switch layout, comments, stale names, and
  redundant nested ARM64 conditions; and
- added small target-local address-plus-`LDAR` primitives for VM and JIT
  publications rather than duplicating acquire sequences or changing common
  code. Source and generated-object contracts pin every converted consumer.

The large ARM64 lifecycle sections in `src/lj_trace.c` were reviewed but not
moved. They span recording, publication, retirement, and unwind lifecycle, and
many existing contracts inspect that file directly. Extracting only part would
split one subsystem and create more maintenance divergence than it removes.
Any future extraction should be a separate mechanical change that moves the
whole lifecycle and updates the source contracts without changing behavior.

## Verification checkpoints

- `tools/ci/arm64_meta_publication_contract.sh`: source and generated-object
  checks pass at `3f109fa2`, including every new unconditional VM acquire.
- Safe ARM64 runtime smoke at `f1fe1f73`: `jit.arch == "arm64"`,
  `jit.os == "OSX"`, `jit.status() == false`, and 64-bit FFI pointers.
- `tools/ci/arm64_jit_fail_closed_gate.sh`: complete pass at `f1fe1f73` in
  about 164 seconds, including ordinary ARM64 and arm64e/BTI publication,
  entry, exits, retirement, flush/reuse, first-side paths, safepoints,
  forwarding-safe `lj_vm_next`, callback-result lifetime, interpreter and JIT
  GC2 contracts, 3,072 compiler combinations, 37 admitted modes per process,
  and 222 default runtime mode executions.
- Exact ARM64 `CALLXS` scalar/lifecycle suite: focused pass at `f1fe1f73` for
  the integer, double, and boxed-pointer certificates, including the native
  GC2 equality/overdue cadence proof. This suite is not part of the
  fail-closed umbrella above.
- Native experimental ARM64 vendored suite: 509 passed at `4d1b6126`.
- Disposable x86_64/Rosetta vendored suite: 509 passed at `4d1b6126`.
- Safe ARM64 no-JIT stock suite: 387 passed at `e2c8778d`.
- ARM64 bootstrap gate: passed at `9044cac5`, including the stock suite,
  TG/root/safepoint/protected-call contracts, threading/coroutines, and 320
  FFI callback rounds across four threads, plus ordinary ARM64 and arm64e/BTI
  interpreter GC2 hard-assist proof.

Known diagnostics observed during validation are the pre-existing unused
`ccall_rawchild_wait` warning, the diagnostic `topofs` warning, and the x86_64
builder's expected preliminary non-GC64 rejection before its configured GC64
build.

## Next review priorities

1. Define and prove the activation policy for legacy closures that retain open
   upvalues into an owner stack before they cross `threading.spawn`/attach.
2. Open only one new JIT or FFI boundary at a time, with independent source,
   IR, snapshot, post-RA, emitted-code, lifecycle, and negative-shape proof.
3. Keep common-file edits narrower than the protocol being added and preserve
   stock LuaJIT control flow wherever no lockless change is required.

## Notes layout

- [`README.md`](README.md): current implemented, verified, and fail-closed
  status.
- [`architecture-constraints.md`](architecture-constraints.md): durable
  correctness and minimal-divergence rules.
- [`archive/aarch64-checkpoints/`](archive/aarch64-checkpoints/): 76 historical
  ARM64 investigation and checkpoint notes.
- [`archive/pre-aarch64/`](archive/pre-aarch64/): 847 earlier LuaJITMT notes.
- [`archive/complaints/`](archive/complaints/): 15 historical environment and
  test-runner reports.

## Primary checks

```sh
tools/ci/arm64_meta_publication_contract.sh
env MACOSX_DEPLOYMENT_TARGET=13.0 tools/ci/arm64_jit_fail_closed_gate.sh
env MACOSX_DEPLOYMENT_TARGET=13.0 \
  LUA_PATH='tests/lib/?.lua;src/?.lua;src/jit/?.lua;;' \
  src/luajit tools/test.lua m7_ffi_callxs_arm64_scalar
env LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet
```

Use focused contracts while developing and the complete fail-closed gate
before making a checkpoint claim. Keep source, proof, and documentation in
separate validated commits, and push each completed slice.

Read [`architecture-constraints.md`](architecture-constraints.md) before
changing VM, JIT, FFI, GC2, publication, or cleanup paths.
