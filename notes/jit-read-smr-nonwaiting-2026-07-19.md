# Nonwaiting JIT trace-reader SMR tranche

Date: 2026-07-19

This b1.2.1 checkpoint removes the looping GC2 SMR admission API from the JIT
reflection, recorder lookup, assembler lookup and optional GDB-JIT metadata
files. It is a bounded tranche, not a claim that trace publication, flush,
retirement or the repository-wide wait inventory is complete.

## Runtime policy

The `jit.util` trace-reflection functions are observational under concurrent
mutation. They already used a one-shot recorder-token acquisition and returned
no result when a peer owned it. They now apply the same rule to SMR: failure to
enter the current trace-body generation is a transient no-result observation.
If the function acquired the recorder token, it releases that token before
returning. This prevents an exclusive reclaimer that also needs the token from
forming a token/SMR dependency cycle.

Reflection does not allocate while retaining trace-body SMR. `tracesnap`
copies its at-most-255 immutable map entries to bounded C-stack storage before
building the result table. `tracemc` first sizes the current observation,
grows the owner TG's managed scratch buffer outside SMR, and then performs a
second one-shot lookup which copies only a body that fits. String interning is
therefore outside both trace SMR and the temporary recorder-token claim; an
allocation error cannot leak a trace pin or strand a reclaimer. The second
admitted interval also converts the mcode address to an integer scalar before
release; no expired trace or mcode pointer value is evaluated afterward.
`traceexitstub` applies the same rule to its borrowed exit-stub address.
`tracek` reduces scalar constants to copied IR words; for `IR_KGC`, whose word
still names a reclaimable child, it additionally acquires an exact GC2
allocation lease under trace SMR, publishes the copied TValue as a Lua stack
root after leaving trace SMR, and only then releases the child lease. A
transient or stale child admission is another no-result observation.

Recorder and assembler lookups have a stronger semantic obligation, but both
are speculative. A failed one-shot SMR admission aborts that recording turn
with the admission-specific `LJ_TRERR_SMRRETRY`, allowing execution to continue
in the interpreter and a later hot turn to record again. The new error was
appended to the catalog so every pre-existing numeric trace-error value remains
unchanged. It gets the same immediate-hotcount policy as `LJ_TRERR_RETRY`, but
only `SMRRETRY` suppresses the TRACE-abort event: ordinary optimizer, stale-body
and table retries remain observable through the existing API. The assembler's
target-return-PC shortcut is optional and simply retains the snapshot PC when
admission loses.

The hot-side probe likewise remains interpreted when admission loses. After a
snapshot has already been restored, a `JLOOP` target revalidation collision
returns to bytecode redispatch. Neither optimization can make an exiting
mutator wait for a trace reclaimer.

GDB-JIT metadata is optional. If a side trace's parent cannot be admitted, its
debug entry is omitted instead of waiting or publishing an object with a
guessed parent stack adjustment. Normal trace execution and retirement are
unchanged.

Debug trace-PC recovery similarly reports no bytecode position on contention.
The x86/x64 final link assembler retries the recording turn. Bytecode dumping
recovers patched root instructions from the prototype's immutable sidecar;
only a compatibility fallback tries trace-body SMR, and that admission is
one-shot with an already bounded live-bytecode resample.

There are now no `lj_gc2_smr_read_enter()` call sites in `lib_jit.c`,
`lj_record.c`, `lj_asm.c`, `lj_asm_x86.h`, `lj_bcwrite.c`, `lj_debug.c` or
`lj_gdbjit.c`. A monotonic source gate protects this completed domain. Blocking
admissions remain in `lj_trace.c` and GC/safepoint lifecycle files and must be
converted with their matching flush/retirement descriptors; this tranche does
not hide that debt behind an allowlist.

## Terminal cleanup

Failed assembler copies now retire as raw unpublished scratch descriptors: they
carry no semantic snapshot/prototype/exit-table graph and use one-shot raw marks
around token-owned list publication. Successful assembly, ordinary cancel and
owner-abort paths publish INTERP+IDLE and release the recorder token before
dispatch repair. First-area `MCODEAL` and max-trace exhaustion likewise defer a
full flush until after that terminal release. An asynchronously aborted
`MCODELM` restart falls through full slot cleanup instead of leaking `J->cur`,
and down-recursion retries only when its restarted recorder remains non-IDLE.

The normal active `TRACE_START` path still installs its recording dispatch
overlay while retaining the token. Any wait/dependency hidden in that active
overlay update is explicit remaining debt; silent-IDLE start exits no longer
share it.

## Evidence

The recorder-token fixture closes SMR in `LJ_GC2_SMR_META_EXCLUSIVE` and calls
every trace-numbered `jit.util` reader. Each call returns without a result,
does not increment the reader count and leaves the recorder token unowned. A
regression to the looping admission API hangs this deliberately closed scope
and is terminated by the focused suite timeout.

The immutable-start-instruction fixture also publishes a real patched `JLOOP`,
closes SMR exclusively, dumps the owning function, reloads the bytecode and
executes it. This proves that bytecode serialization uses the prototype
sidecar rather than waiting for or dereferencing the trace vector. Serialization
now acquire-loads every mutable `BCIns` independently and always runs a captured
JIT opcode through sidecar/unpatch recovery. It no longer races a bulk `memcpy`
or a stale `PROTO_ILOOP`/prototype-trace fast-path decision against publication
and flush.

The same fixture exposed a stale XPOLL assertion after mutable-global tracing
became conservative. A safe runnable side trace may now end at the interpreter
instead of using `LJ_TRLINK_ROOT`; the test now checks the actual invariant, an
exact snapshot on a non-loop XPOLL in any runnable side-trace topology.

## Remaining event debt

The exact closed-gate `SMRRETRY` path cannot safely call arbitrary TRACE-abort
handlers while it still owns the recorder token, because a handler may enter a
reader needed by the already-admitted writer. Other abort events retain their
historical visibility. They still run token-held and therefore retain a more
general close-after-check dependency race if a metadata-exclusive owner closes
after event preparation begins. Moving those callbacks requires preserving
`jit.dump` and user abort-observation ordering and is intentionally recorded as
follow-up debt rather than hidden by suppressing ordinary `RETRY` events.
