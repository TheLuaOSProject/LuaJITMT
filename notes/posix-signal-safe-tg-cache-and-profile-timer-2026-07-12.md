# POSIX TG signal cache, owner-dispatch handoff, and image lifetime

This checkpoint hardens the x86-64 Linux/macOS `SIGPROF` path without claiming
that the wider universe-lifetime migration is complete. The installed handler
no longer resolves compiler TLS/TLV state and no longer mutates a hook mask or
dispatch table. It performs a process-incarnation lookup, publishes sample
atomics to the interrupted TG, and returns. The owning VM consumes a separate
request word in normal context before installing the profile dispatch overlay.

## Non-reusable process incarnation

Signal-cache cells are immutable hash nodes keyed by
`{fork generation, PID, pthread_self}`. The generation starts at one and is
advanced by a registered `pthread_atfork` child callback. It never wraps:
advancing `UINT64_MAX` permanently poisons signal lookup in that process copy.
The callback zeroes the cached PID throughout the transition and resets copied
`BUILDING` states, so a child cannot wait for a builder thread which vanished
at `fork()`.

Linux additionally requires a dedicated anonymous page successfully marked
`MADV_WIPEONFORK` before signal-cache admission. The kernel zeros this page for
libc and direct syscall fork/clone paths. A handler accepts the cache only when
the page contains READY. Owner context claims a zero page, advances the exact
generation, updates the cached PID, and release-publishes READY. Nested raw
forks therefore remain rejected even if no intermediate process enters libc or
LuaJIT, and forced ancestor PID plus `pthread_self` numeric reuse cannot revive
an inherited cell. If the mapping/advice is unavailable, signal admission and
profile start fail closed. macOS has no equivalent wipe-on-fork primitive in
this checkpoint; its exact proof covers the supported `fork()`/`pthread_atfork`
path and cold PID-mismatch repair, not adversarial direct kernel traps with a
full ancestor PID/thread-identity cycle.

The witness is unmapped on an ordinary pre-activation DSO unload (including a
failed image pin). Once the unregistrable atfork callback is READY, the caller
has already made the image process-stable; the page is then deliberately left
mapped until kernel process teardown so an already-selected handler can never
race an unmap.

The handler still calls eagerly relocated `getpid` on every lookup. This is a
mandatory backstop, not merely another hash component. A raw syscall fork, a
foreign runtime which bypasses the registered callback, or a signal delivered
before the child callback completes sees either a non-READY Linux witness or
`actual_pid != cached_pid` and returns `NULL` before reading an inherited cell.
The next cold admission repairs that case, advances the generation once, and
resets copied key/atfork builders. Sibling children may have the same numeric
generation, but their newly allocated cells live in private post-fork address
spaces; every inherited cell has the preceding generation.

Cell bucket heads are release-CAS published after all immutable identity and
`next` fields are initialized. Handler traversal uses acquire loads, performs
no allocation, key access, lock, retry loop, or scheduler call, and uses only
the eager `getpid`/`pthread_self` function-address relocations plus atomics.

## Exact and transitional raw tags

The exact cache word remains `body|1`. `lj_thr_get_tg_signal()` accepts only
that tag, so an exact-lifetime consumer cannot accidentally accept a raw
binding. Exact install publishes TLS ownership before the mirror; exact clear
removes the mirror before TLS relinquishes ownership; swap publishes the new
TLS owner and mirror while both fungible registry counts remain operation-owned.

Production lifecycle callers still use the ABI-compatible raw TG setter. To
avoid silently breaking `jit.profile` until that migration lands, raw setter
boundaries also publish a distinct profiler-only `body|2` mirror. The old mirror
is cleared before raw TLS changes; a new mirror is published only after the new
TLS word. `lj_thr_get_tg_profile_signal()` accepts exact or profiler-raw tags,
while the exact getter continues to reject raw. A raw cell admission failure
does not break VM attachment: it leaves ordinary raw TLS installed. A later
setter retries, and profile start now also requires retryable admission of the
current TLS binding, with its TG exactly matching the `lua_State` passed to
`luaJIT_profile_start`, before it installs a handler. An active profiler cannot
silently remain detached from or misrouted away from its starting thread.

The raw tag is explicitly temporary. Its safety argument is limited to a
signal interrupting the same OS thread: the interrupted attach/detach operation
cannot resume and release its TG while the handler is running, and detach clears
the mirror before retirement. It is not an exact TG lease and it is not a
universe lease. Forced thread termination and unproven remote raw-holder
reclamation remain outside this checkpoint. Full production exact admission
must delete tag 2 before release.

## Atomic handler to owner-context dispatch

`TGState.profile_request` is adjacent to the existing 32-bit safepoint poll
word. The x64 VM tests the aligned pair with the same single not-taken qword
compare used at existing poll edges. This preserves the fast path while making
an asynchronous profile request enter `lj_safepoint_ack()`. Ordinary
`lj_safepoint_poll(L)` also recognizes the request word, so C-owner polling
uses the same consumer rather than waiting specifically for a VM edge.

JIT loop backedges now emit the same qword comparison whenever this VM's
SIGPROF policy is active. Start release-publishes that recording policy and
flushes every pre-policy trace before arming the timer; concurrent recordings
either predate the flush or observe the policy and contain XPOLL. Stop disarms
and drains first, clears the policy, then flushes poll-bearing traces. Thus an
otherwise infinite single-TG numeric trace exits for the owner callback while
ordinary unprofiled single-thread traces retain their previous no-poll fast
path. Loop optimization may be disabled through the public JIT options, and
trace graphs can form cycles outside one optimized IR loop, so the recorder
also appends a guarded tail XPOLL to every policy-requiring trace which will
not receive the optimized IR_LOOP poll. `t-profile-single-trace.lua` exercises
both the default optimizer and level zero; without either qword XPOLL its
traced loop reaches the suite timeout and no callback runs.

Internal policy retirement is deliberately eventless. Calling a user TRACE
`"flush"` handler while profiler state is STARTING or STOPPING lets a handler
which reenters `profile.start()`/`profile.stop()` self-wait forever. This is a
documented safety divergence from the incidental TRACE event previously
observable for `l`/`f` profiler-mode retirement; explicit public `jit.flush()`
events are unchanged. The internal flush still runs behind an outer protected
call because allocation and shutdown edges may unwind. Start clears `poll_g`,
resets `prof_mode`, and returns the profiler to IDLE before rethrowing. Stop
completes timer/handler drain, callback and buffer teardown, clears the global
pointer, and publishes IDLE before rethrowing. A flush which merely reports
that a GC hook currently forbids retirement makes start fail closed before
`sigaction`; on stop, an unflushed residual XPOLL is safe and only retains the
slower poll check until a later successful flush.

The Lua `jit.profile.start` wrapper protects the low-level start call after it
anchors the hidden callback coroutine and function. If retirement or another
start edge throws, it removes both registry roots before rethrowing; low-level
state rollback and high-level callback-root rollback therefore form one error
boundary instead of leaking the hidden callback state until a later start.

The handler linearization is:

1. exact/transitional signal lookup validates the current process incarnation;
2. a nonwrapping in-flight-handler count is acquired;
3. ACTIVE/global/TG eligibility is revalidated;
4. `profile_samples` is atomically incremented and `profile_vmstate` stored;
5. release-store `profile_request = 1` publishes the request;
6. the in-flight count is released and caller `errno` restored.

No handler instruction writes `hookmask`, `hookmask_th`, or `dispatch[]`.
The owner acquires/exchanges `profile_request` to zero in
`lj_profile_owner_poll()`, revalidates ACTIVE and the exact TG/global pairing,
then updates the TG hook mask and dispatch overlay in ordinary VM context. A
second ACTIVE check closes the race with stop: either stop clears after the
owner publication, or the owner observes STOPPING and clears its own overlay.
The profile callback exchanges the request before clearing the hook, so a
later signal leaves another VM poll edge. A narrowly timed included sample can
cause one harmless empty overlay cycle, but no request or sample is lost.

The TG exact lease (or temporary same-thread raw argument) protects only the TG
body. `ProfileState.signal_handlers` and ACTIVE -> STOPPING protect profiler
state and its published global pointer until stop drains all handlers which
could have observed ACTIVE. They do **not** constitute the future whole-universe
lease. The exact universe token must cover `global_State`, registry
infrastructure, and teardown before this can be the final lifetime proof.

## Timer and containing-image lifetime

Start registers a profile-specific atfork child callback and permanently pins
the image containing `profile_signal` before the first `sigaction`. On Linux,
the main executable is proven directly from its unnamed `PT_LOAD` program
headers before consulting the dynamic loader; this also covers PIE, deleted,
and fully static main images for which `dladdr` has no usable path. On macOS,
the `dladdr` base is compared with dyld image zero. A loadable DSO must
successfully `dlopen` itself, and the returned handle's link-map/symbol base
must match the original `dladdr` base so pathname replacement cannot pin the
wrong image. Failure aborts profile start before handler installation. The
successful handle is intentionally never released.

Cell/key admission by itself does not register the TG-cache atfork callback.
`profile_timer_start()` first seeds/repairs process identity (which may create
the Linux witness page but registers no callback), then pins the image, then
activates both the TG-cache and profile child callbacks. Seeding before any
image-pin `BUILDING` publication lets a missed/raw child identify and reset a
copied builder. Pinning before activation matters because POSIX has no atfork
unregister operation: a failed pin must leave a DSO unloadable without a latent
callback into unmapped code. The standalone exact-cache fixture activates from
its non-unloadable main executable explicitly.

Permanent pinning is required even after a successful disarm and disposition
restore: userspace cannot prove that the kernel has no already-selected or
latent entry to the previous handler address. A failed disarm/restore makes
the need obvious, but successful stop is not a safe point for unmapping either.
This deliberately trades one DSO reference for process-lifetime code safety and
does not break a static/main-executable embedding.

Handler installation precedes timer arming. Failed arm restores a newly
installed old action when possible; failed rollback leaves the pinned handler
installed but inert. Stop disarms before restoring. Failed disarm or restore
likewise leaves the pinned handler inert. A failed start drains handlers which
entered during STARTING before callback/buffer data is reset and IDLE is
published. The profile atfork callback resets an inherited handler count and
copied cold builders; cold PID repair supplies the same reset for a missed/raw
fork. The profiler also snapshots the signal-cache generation, so Linux
WIPEONFORK repair clears a phantom handler count even when a descendant is
forced to reuse both cached numeric PIDs. All handler exits restore `errno`.

## Deterministic and artifact coverage

`t-posix-signal-safety.c` covers retryable Linux fork-witness and key/cell
admission failures, exact install, swap and clear, transitional raw rejection
by the exact getter, generation
change under forced PID/thread reuse, saturation poison, cold missed-fork
repair, an uninterrupted nested `SYS_fork` chain with forced identity reuse,
nested libc fork with copied builders, pthread destructor clearing, pin
failure before `sigaction`, child-incarnation profile-cell admission failure
and retry, a GC-hook-vetoed pre-policy trace flush, injected initial/rollback/
stop retirement unwinds with post-error restart, every timer syscall rollback/
recovery case, atomic request publication followed by owner-context dispatch
installation, a paused handler which forces stop to drain, and profile atfork
count repair. The Lua trace regression also attaches a reentrant TRACE handler,
proves profile start/stop do not invoke it, and proves public `jit.flush()` still
does. The focused C fixture also injects a Lua-wrapper start failure and checks
that both hidden callback registry entries were removed.
`t-profile-single-trace.lua` separately requires callback delivery from an
otherwise unbounded single-TG JIT trace.

The same source builds as an unlinked loader. Separate processes exercise a
helper DSO through pre-open pin failure, failed handle/base verification,
successful start/stop/dlclose, and failed disarm/dlclose followed by a real
`SIGPROF`. They verify both pre-open pin
failure and a rejected post-`dlopen` image match close their references and
permit unload; each failure process then forks to prove no unregistrable
callback was installed into the unloaded image. Every installed-handler path
retains the DSO.

ELF and Mach-O gates inspect static/PIC objects and the final helper DSO. They
require eager getpid/pthread relocations, reject TLS/TLV/key/allocator/loader/
scheduler/dispatch calls in the reachable handler/getter path, require only the
errno accessor plus profiler signal getter from the handler, check the combined
x64 poll word and owner consumer, and retain the ordinary one-`%fs`-load
`lj_thr_get_tg()` hot path. The Linux artifact gate also links and executes the
entire fixture as a fully static executable, proving the main-image `PT_LOAD`
path does not depend on `dladdr` or a loadable pathname.

Validation at this checkpoint: the focused Linux fixture passes repeated
optimized runs including initial, failed-arm rollback, stop, and Lua-wrapper
retirement unwind edges, focused ASAN+UBSAN and GCC TSAN runs, all four DSO
lifetime modes, the stripped
final-ELF artifact gate, and the fully static executable. A production helper
delivers SIGPROF from otherwise unbounded default-optimizer and optimizer-zero
single-TG traces; both machine-code dumps contain the qword XPOLL, while the
equivalent unprofiled traces have no poll. Strict x86-64 osxcross compilation
passes for the signal sources, recorder policy, no-JIT
profile source, and both test modes; the rebuilt stripped final-Mach-O artifact
gate passes. Strict MinGW x86-64 compilation passes for the affected sources
(apart from suppressing the pre-existing `GetProcAddress` function-cast
diagnostic in `lj_profile.c`). Full-tree sanitizer builds and Darling/Wine
runtime suites remain required once the concurrent GC/cdata source tranche is
stable.

The final blocked-TG and VM-event flush gates also exposed a separate JIT/GC2
regression while the safepoint leader was reclaiming retired traces: the
exclusive SMR writer re-entered `lj_gc2_smr_read_enter()` from
`trace_preservebody()` and waited on itself while a blocked TG waited for the
leader acknowledgement. Retired-trace preservation now has an explicit
exclusive-reclaimer path which asserts the writer context plus recorder token
and does not enter a reader lease; ordinary publishers and root scans retain
the reader-protected path. Both `t-profile-blocked-tg.lua` (JIT off and on) and
`t-jit-vmevent-flush.lua` pass after that split.

## Remaining release blockers

1. Delete the transitional raw profile tag after all main/spawn/foreign/worker
   bindings enter and leave with exact TG and universe tokens.
2. Add the whole-universe lease; an exact TG token alone does not pin `tg->gl`
   or teardown-only subsystem state.
3. Either add a Darwin kernel-backed fork-incarnation witness comparable to
   Linux `MADV_WIPEONFORK`, or formally exclude direct fork traps which bypass
   libSystem's supported atfork protocol.
4. Replace cold `mmap`/`madvise`, `pthread_atfork`, `pthread_key_create`,
   `malloc`, `pthread_setspecific`, and BUILDING `sched_yield` paths with the
   final preallocated nonblocking admission design.
5. Bound or replace the never-reclaimed immutable cell chains.
6. Make clean pthread exit transfer/clear/release its exact lease instead of
   clearing visibility and leaking the fungible count.
7. Replace `profile_signals_wait()` retry-yield draining with deferred teardown
   and last-handler completion publication; a stalled handler can currently
   delay stop/close.
8. Give handler-count saturation a formal terminal profile state. The current
   CAS refuses a wrapping entry, but exhausting 32 bits is not yet integrated
   with deferred teardown.
9. Keep the permanent image reference by design, and account for it explicitly
   in embedding/resource documentation rather than treating it as reclaimable.
