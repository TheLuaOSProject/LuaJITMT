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
