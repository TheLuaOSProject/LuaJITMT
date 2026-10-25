# Darwin upstream GC2 runtime checks

The upstream huge-tail and remote owner-root completion fixtures now have
native Darwin ARM64 execution through `m3_darwin_gc2_runtime`. Linux paths and
GNU linker wrapping remain unchanged. This case builds an assertion/helper
profile, runs both fixtures, and restores the caller's ARM64 bootstrap or
experimental profile. The shared test build helper accepts an explicit restore
XCFLAGS value; callers that omit it retain their previous behavior.
The complete ARM64 bootstrap gate requires this case, including restoration
of its disabled-JIT assertion profile before the remaining stock and FFI tests.

Darwin wrapper translation units compile the actual arena and safepoint sources
with selected call names redirected to the existing test wrappers. They replace
only those objects when linking the static archive. Production objects and APIs
are unchanged. The arena test covers allocation failure, zero allocator calls,
physical mapping geometry, final payload bytes, exact logical bounds, reader
retention, resize, transfer and final unmap. Its original mmap64 wrapper uses
Darwin's 64-bit mmap/off_t ABI.

The remote completion fixture runs all six upstream modes. It still performs
the real root scan, requests, native transitions, SSB publication, phase changes
and state claims. For mode 1, a test wrapper records the exact Darwin ulock wait
on the owner's poll word with value 1 and no timeout. The observer requires
Mach thread_info to report TH_STATE_WAITING while that exact wait is active
before it releases the paused remote executor. The actual __ulock_wait call is
preserved. This does not substitute a userspace sleep for kernel-wait evidence.

## Native evidence

On the resolved upstream candidate based on primary `25275a13`, all huge-tail
cases and remote modes 0 through 5 passed. The registered suite case then
passed using its minimal matching helper flags and successfully restored the
experimental profile. These fixtures exercise upstream arena/safepoint code;
they do not depend on the later NUM admission-only changes in `348dee94`.
The registered case also passes on the final `8893ffc4` merge candidate when
started from the safe ARM64 profile. Afterwards `jit.status()` is false and
`jit.opt` is absent, confirming that restoration path as well.

Two separate disposable negative builds changed only one production behavior:

- Removing the 16-byte wide-stamp tail from huge mapping size caused the
  final-payload-byte case to fail its exact 128-bit stamp comparison (SIGABRT).
- Removing the wake from safepoint_clear_poll left mode 1 blocked after the
  observer had confirmed the actual Darwin kernel wait. The fixture's alarm
  fired (SIGALRM); the correct source completes all modes.

Temporary local logs are `/tmp/luajitmt-upstream-huge-tail.log`,
`/tmp/luajitmt-upstream-remote-completion.log`,
`/tmp/luajitmt-upstream-darwin-runner.log`, and
`/tmp/luajitmt-negative-{huge-tail,remote-completion}.log`.
The isolated source location is in `/tmp/luajitmt-upstream-native-workdir`.

## Local duplicate and native-return holds

The same registered case now also requires the upstream local-native duplicate
scan test and all four native-root-hold modes on Darwin. Their original Linux
paths remain unchanged. The Darwin paths retain real owner/global root scans,
SSB flushes, and native transitions. They observe the exact poll-word ulock wait
with Mach thread state before releasing the paused executor. The duplicate
fixture redirects only the parser-root observation point inside the actual
GC2 source; its wrapper still calls the real parser scanner.

All five added executions passed on `cac092fd` plus these test changes, together
with the huge-tail and six completion modes. The runner restored the safe
profile. Independent source review found no weakened ownership, marking,
completion or source-preservation assertions.

Separate disposable negative controls failed with SIGABRT as intended:

- Removing the `native_parked` restriction from local completion allowed the
  local duplicate to clear poll while a remote traversal still held its TG;
  the fixture detected teardown during that traversal.
- Bypassing the consumed-poll wait made each of the four native-return modes
  return while poll was still set. Each failed the original poll-clear check.

The earlier memory summary's mention of `teardown-during-scan=1` and
`lj_tg_poll_acq(ctx.owner)==0` as Darwin rerun failures was an incorrect
classification of those negative-control outputs. The captured positive run
and a fresh isolated repetition pass; no fixture or assertion change was
needed. On 2026-10-25, an archive of `c3cbb04a` was built under
`/tmp/luajitmt-darwin-root-repeat` with the Darwin assertion/helper profile.
The local duplicate fixture passed three consecutive runs, each observing the
exact consumed-poll ulock wait and reporting `teardown-during-scan=0`. The
native-return fixture passed modes 0 through 3 in three consecutive rounds
(12 total), each observing the real ulock wait and returning with the root
marked and source unchanged. This is focused repeat evidence, not a substitute
for the complete Darwin gate.

No production behavior was changed by these test ports. Positive evidence is
in `/tmp/luajitmt-darwin-holds-runner.log`; negative logs are
`/tmp/luajitmt-negative-local-hold.log` and
`/tmp/luajitmt-negative-native-{0,1,2,3}-hold.log`.
