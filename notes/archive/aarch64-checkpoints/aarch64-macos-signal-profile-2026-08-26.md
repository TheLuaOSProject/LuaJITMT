# Native macOS ARM64 signal cache/profile checkpoint (2026-08-26)

## Frozen scope and claim

This checkpoint admits the existing process-stable TG signal cache and
TG-local `SIGPROF` publication on **desktop macOS ARM64 only**, under the
interpreter bootstrap:

```text
LUAJIT_MT_ARM64_BOOTSTRAP
LUAJIT_DISABLE_JIT
```

The existing x86/x64 profiler predicate is preserved. iOS remains excluded.
The coordinated compile-time invariant is one-way: every `SIGPROF` target that
enables `LJ_THR_TG_SIGNAL_CACHE` must also enable `LJ_PROFILE_TGLOCAL`. The
reverse is intentionally false for the established 32-bit x86 legacy backend,
which is TG-local without this signal cache.

This is not an ARM64 JIT checkpoint. It makes no ARM64 trace, recorder, compiled
loop, or XPOLL claim. The validated consumer is the real ARM64 interpreter VM
poll edge only.

## Why the ARM64 admission is narrow

The signal getter eagerly calls cached `getpid` and `pthread_self` entry cells,
then searches the process/generation/owner cache with inline lock-free atomics.
The profiler handler saves/restores `errno`, obtains the current TG through the
profile signal getter, and publishes only samples plus `profile_request`.
Normal-context owner polling consumes the request and installs the profile
dispatch overlay.

`pthread_self()` in a signal handler is **not** a portable POSIX guarantee. This
port depends on the implementation-specific Apple ABI and is positive evidence
only for the current Darwin host. The native runtime fixture and generated
Mach-O checks validate that dependency here; they do not generalize it to iOS
or other pthread implementations.

## Generated-code gate

`tools/ci/m4_posix_signal_macho_artifacts.sh` now accepts thin `arm64` and
`x86_64` Mach-O inputs and retains the x86 recorder/XPOLL checks only on x86.
For each architecture it proves:

- each exact/profile signal getter has exactly two calls, both indirect through
  the eagerly initialized `getpid` and `pthread_self` function cells;
- the getter objects have no branch relocations and exactly one eager unsigned
  relocation to each libc/pthread entry cell (the ARM64 cell loads themselves
  use their expected PAGE21/PAGEOFF12 relocation pair);
- each profiler handler object has exactly two branch relocations/calls, to
  `___error` and `lj_thr_get_tg_profile_signal`;
- the final exported helper getters and handler preserve those call counts;
- the bounded getter/handler paths contain no TLV/TLS access, allocation,
  loader, atfork, scheduler, dispatch-update, atomic-library, AArch64 runtime,
  stack-check, or sanitizer helper edge; and
- the checked objects/final image do not import atomic/runtime helper families
  such as `__atomic*`, `__sync*`, or `__aarch64_*`. Stack-check and sanitizer
  helpers are forbidden in the bounded getter/handler bodies, but unrelated
  hardening elsewhere in the image does not fail this call-graph gate.

The ARM64 source-side half of this gate checks the interpreter poll macro and
its `profile_request` load. It deliberately does not inspect or assert an ARM64
JIT/XPOLL path.

## Runtime coverage

The ARM-specific aggregate suite rebuilds with the two signal test-helper
families plus the ARM64 bootstrap/JIT-disable flags, then runs:

1. the Mach-O artifact gate;
2. `t-posix-signal-safety.c`, including exact cache/process/fork lifecycle,
   timer rollback, handler drain, request publication, and callback dispatch;
3. the four unlinked DSO-loader modes (`pin-failure`, `pin-mismatch`, `success`,
   and `stop-failure`); and
4. `t-vm-safepoint.c` against the helper archive.

The last fixture sets only `tg->profile_request`, calls a real Lua fixed-function
entry (`BC_IFUNCF`), and proves that the ARM64 VM poll consumes that request
without fabricating a handshake poll or epoch change. This is the integrated
interpreter edge required by the checkpoint.

Darwin documents that an image containing compiler TLS is not unloaded after
`dlclose`. LuaJIT's full DSO contains normal-path compiler TLS even though the
bounded signal getter/handler paths do not reach TLV/TLS. Therefore the two
negative DSO modes cannot use `RTLD_NOLOAD` as a Darwin unload oracle. They
instead prove `!pinned`, `!installed`, successful `dlclose`, and a safe
post-close fork. The success/failure-after-install modes additionally prove the
logical permanent pin and safe real `SIGPROF` after `dlclose`. Linux retains the
strong negative `RTLD_NOLOAD` assertion.

## Native validation

Host: macOS 26.5.1 (Darwin 25.5.0), Apple Silicon ARM64, Apple clang 21.0.0.

The aggregate command was:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 \
  LJ_TEST_ROOT=/Users/frityet/Projects/LuaJITMT \
  src/luajit tools/test.lua m4_posix_signal_arm64_gate
```

Exact result:

```text
m4_posix_signal_macho_artifacts OK: arm64 handler/getters are publication-only; interpreter poll only, no JIT/XPOLL claim
t-posix-signal-safety OK: exact cache and timer lifecycle verified
DSO pin-failure: exit 0
DSO pin-mismatch: exit 0
DSO success: exit 0
DSO stop-failure: exit 0
t-vm-safepoint OK: loop, entry, return and unwind polls acked
M4 macOS ARM64 signal/profile interpreter-poll gate passed
ok m4_posix_signal_arm64_gate
```

The suite restored the ordinary assert-enabled ARM64 interpreter bootstrap.
The restored artifacts are thin ARM64 and report `jit.os == "OSX"`,
`jit.arch == "arm64"`, `jit.status() == false`, and `jit.opt == nil`.

These signal cases mutate the source-tree build and intentionally reset it to a
canonical profile rather than claiming to recover arbitrary incoming
`XCFLAGS`: the assert/no-JIT bootstrap on native Apple ARM64 and the repository
default elsewhere. They also reject a test compiler whose detected target
architecture differs from the running LuaJIT, because such a build could not
be exercised or reliably restored by the same process.

## x86-64 preservation validation

A thin x86-64 helper build was produced with `-arch x86_64` and run under
Rosetta. Results:

```text
m4_posix_signal_macho_artifacts OK: x86_64 handler is publication-only
t-posix-signal-safety OK: exact cache and timer lifecycle verified
x86_64 DSO modes OK: pin-failure pin-mismatch success stop-failure
```

The artifact gate also retains the existing combined TG poll-word, profile poll
policy, two recorder XPOLL emission sites, no-event trace flush, and x86-64
compare-emission source assertions.

## Remaining boundary

The exact TG cache lease/raw-tag and future whole-universe lifetime limitations
documented by the original POSIX signal checkpoint remain unchanged. This
checkpoint establishes the native desktop Darwin ARM64 signal publication
tranche; it does not close those broader lifetime proofs and does not widen the
supported ARM64 execution contract beyond the no-JIT interpreter bootstrap.
