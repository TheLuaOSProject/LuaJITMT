# Apple ARM64 interpreter safepoints (2026-08-26)

## Claim boundary

The Apple ARM64 interpreter now consumes TG-local handshake polls at bounded
VM progress edges and at the protected return/unwind boundary. This checkpoint
is enabled by the existing `LUAJIT_MT_ARM64_BOOTSTRAP` profile, which still
requires `LUAJIT_DISABLE_JIT`. It does not claim ARM64 trace entry, JLOOP,
trace exit, XPOLL lowering, or JIT safepoint parity.

The ordinary `IFORL`, `IFUNCF`, and `IFUNCV` handler bodies contain their polls
unconditionally. This is important for a future JIT-capable ARM64 binary whose
JIT is disabled per function or at runtime: `FORL`, `FUNCF`, and `FUNCV` fall
through to those interpreter bodies. The separate `JFORL`, `JFUNCF`, and
`JLOOP` paths remain deferred.

## Acquire and helper contract

`TGState.poll` and `TGState.profile_request` are distinct 32-bit release
publications. ARM64 acquire-loads each field at its natural width with `ldar`
and ORs the results. It deliberately does not copy x64's adjacent qword test,
which would introduce a mixed-width atomic contract on a weakly ordered target.
Static assertions pin both dispatch-relative offsets, their alignment, and
their adjacency.

The common `vm_safepoint` path publishes `L->base` and `SAVE_PC`, calls
`lj_safepoint_ack_check(L)`, reloads `BASE` in case the stack moved, and resumes
through normal TG dispatch. It is reached after the target PC and any result or
control-variable publication are complete from:

- the positive `ITERN` and non-nil `IITERL` paths;
- `ILOOP` and `JMP` progress edges;
- the taken integer and floating-point numeric `IFORL` backedges;
- ordinary fixed-argument and vararg interpreter function entry, after missing
  parameters, moved fixed arguments, the new base, and the constant base are
  complete.

Leave-to-C and unwind-to-C use direct helper calls because they cannot resume
with `ins_next`. Leave checks before restoring the previous C frame, keeping a
valid assembler frame available if STOPREQ throws. Unwind checks after
reconstructing the TG dispatch pointer and before publishing VM state C; it
saves and restores the incoming error status in `SAVE_ERRF` so an observational
acknowledgement cannot replace the protected-call result.

## Profiling exclusion

The VM already reads `profile_request` separately so its dispatch ABI will not
need another mixed-width transition later. This is not an ARM64 profiling
publication claim. `LJ_THR_TG_SIGNAL_CACHE` remains restricted to x64 Linux and
macOS because only those pthread identity and generated-code contracts have
been verified. Consequently `LJ_PROFILE_TGLOCAL` remains disabled on ARM64 and
the POSIX profiler retains its legacy signal path there. Widening
`LJ_PROFILE_TGLOCAL` without first porting the process-stable exact signal cache
would make the signal handler depend on unsupported ownership lookup.

## Validation

On the native Apple ARM64 host, an assert bootstrap build completed with:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src -j8 \
  'XCFLAGS=-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
```

The generated thin Mach-O `lj_vm.o` contains nine separate poll/request load
pairs: seven interpreter progress sites plus leave and unwind. Each pair is an
`add` of dispatch-relative offsets `0x808` and `0x80c` followed by 32-bit
`ldar`. The object exports `lj_vm_safepoint`, imports
`lj_safepoint_ack_check`, and has exactly three `BR26` relocations to that
helper: common redispatch, leave, and unwind.

The strengthened `t-vm-safepoint.c` fixture passed against the bootstrap
archive. It covers fixed and vararg entry, ordinary loop and iterator progress,
an asynchronously published STOPREQ inside a non-terminating numeric `FORL`,
return and return-after-publication, and error unwind with the original error
message and protected-call status retained. The existing ARM64
root-publication, metamethod, and `ISNEXT` runtime fixtures also passed against
this build.

## Remaining work

- A JIT-capable ARM64 runtime build is still rejected by `lj_arch.h`, so the
  unconditional interpreter-label behavior has object/source evidence but
  cannot yet be exercised with runtime `jit.off()` on ARM64.
- `JFORL`, `JITERL`, `JLOOP`, trace entry/exit/rethrow, and emitted XPOLL are
  part of the later ARM64 JIT port.
- ARM64 TG-local SIGPROF publication requires a separate exact signal-cache
  port and signal-safety gate.
- The full bootstrap, sanitizer, amalgamated, and sustained multithreaded gates
  remain the aggregate checkpoint responsibility; this focused slice does not
  by itself close the P1 exit gate.
