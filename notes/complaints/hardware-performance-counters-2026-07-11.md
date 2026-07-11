# Hardware performance counters are unavailable

## Impact

Wall-clock benchmarks still run, but the repository cannot currently measure
cycles, instructions, branches, cache misses, or derive IPC. That weakens the
performance-regression gate for changes intended to match or beat stock LuaJIT.

## Evidence

- `/proc/sys/kernel/perf_event_paranoid` is `2`.
- The capability bounding set excludes `CAP_PERFMON` and `CAP_SYS_ADMIN`.
- `perf stat -e cycles,instructions true` fails with:

  ```text
  No permission to enable cycles event.
  ```

## Requested environment change

Expose hardware perf events to this container, preferably with
`CAP_PERFMON` and a host/container `perf_event_paranoid` setting that permits
the unprivileged measurements. `CAP_SYS_ADMIN` or a privileged container also
works but grants substantially more authority than perf itself needs.

This is an environment limitation. It does not block correctness testing, but
it blocks the strongest local performance diagnostics.
