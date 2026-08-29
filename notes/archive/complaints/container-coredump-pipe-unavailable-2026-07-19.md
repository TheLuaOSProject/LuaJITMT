# Container core-dump pipe is unavailable (2026-07-19)

## Impact

An intermittent stock-suite `SIGSEGV` could not be post-mortem inspected. The
exact clean retry and the full default/no-JIT matrix passed, so there is no
reproducible failure to diagnose, but a future one-shot native crash would lose
its most useful artifact in the same way.

## Environment evidence

- `ulimit -c` is `unlimited`.
- `/proc/sys/kernel/core_pattern` pipes crashes to
  `/usr/lib/systemd/systemd-coredump ...`.
- The container has neither `coredumpctl` nor `journalctl` available.
- `/var/lib/systemd/coredump` does not exist in the container.
- No ordinary `core` file was written in the crashing process's working
  directory.

## Requested resolution

Please expose the host/systemd coredump store and `coredumpctl` to the
container, or configure this container's `kernel.core_pattern` to write an
ordinary workspace-accessible core file. A per-job writable path such as
`/tmp/core.%e.%p` is sufficient.

Running a suspected command under `gdb` remains a usable deliberate fallback,
but it cannot recover an already-completed intermittent crash and changes its
timing.
