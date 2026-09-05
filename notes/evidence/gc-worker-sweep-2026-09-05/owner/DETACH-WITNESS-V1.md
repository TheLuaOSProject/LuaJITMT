# Candidate 1 native-worker detach witness

Both real worker RESET_ALLOC and SCAN_ROOTS actions can overlap private detach.
The fixture starts a real explicit cycle with the ordinary public GC2 phase
functions, reaches SWEEP with no synthetic phase/certificate stores, then starts
two real workers. Linker wrappers pause only existing calls. One worker pauses
at its real handshake entry; the other responds to the ordinary pool STOP and
pauses inside lj_tg_detach at lj_gc2_flush_ssb_detach, after the detach poll.
The joining main caller keeps the original native stop/join protocol.

Both candidate-detach-v1-{0,1}.stdout show worker tid3 at DETACHING/native1,
reqmask0/poll0. The first worker then consumes the target's real RESET_ALLOC64
or SCAN_ROOTS16 request, leaving poll1. Its actual remote-private function entry
pauses while the leaving worker resumes private detach and reaches RETIRED,
actor4294967295, native0/poll0 before the remote action may return. The two
runtimes report the observed overlap as exit2 after normal join and cleanup.
No UAF or allocation corruption is claimed by this particular schedule: it
proves that the consumed-poll stability interval does not cover worker detach.

The old worker startup publishes in_native depth1 and never closes that native
scope before lj_tg_detach. Detach polls once, then flushes private SSB/accounting,
frees tmpbuf and clears root/native/actor publications. DETACHING remains
borrowable; its registry state does not exclude this admitted remote action.

Smallest next source repair: call existing lj_native_leave_tg on the physical
worker before lj_tg_detach. At the worker's startup depth1 this closes native,
fences, and services its own poll. A consumed request holds it before private
teardown until the active leader finishes; a new request after the native close
cannot execute remotely. Detach's ordinary poll or DEAD retirement still owns
its counted slot. No borrower is cancelled and no poll/action hold is weakened.
This must be tested against the exact formerly overlapping schedules.

Caller-side candidate-stop-v3 passes eight controls: RESET/SCAN, workers1/2,
L-aware/no-L stop API. Those passes do not repair the detach witness. The first
fixture version's four SCAN failures incorrectly asserted startup actions must
be0 even when startup acknowledged a legitimate RESET request. Version2's two
SCAN worker1 timeouts omitted the owner poll for the preceding RESET before the
target entry could be reached. All original sources/binaries/outputs remain.
Version3 records prior actions and polls only before releasing the paused target
entry; the target is really pending and unacknowledged before stop is invoked.

No broad runtime acceptance or integration is claimed. Candidate1 runtime,
patch and frozen source remain unchanged; the shutdown repair belongs in a new
source generation. No fair-constructor repair is included.
