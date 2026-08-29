# Darling cannot run in the current container

## Impact

The macOS x86-64 target can be cross-compiled with the installed osxcross SDK,
but it cannot be executed locally under Darling. This blocks the requested
Darling runtime gate and leaves macOS behavior dependent on external CI.

## Evidence

- `/usr/bin/darling` is installed.
- `/tmp/osxcross-sdk/MacOSX14.2.sdk` is present.
- `darling shell /usr/bin/true` fails with:

  ```text
  Cannot unshare UTS and IPC namespaces to create darling-init: Operation not permitted
  ```

- `unshare --uts true` also fails with `Operation not permitted`.
- The container has neither `CAP_SYS_ADMIN` nor `CAP_SYS_PTRACE`, seccomp is
  active, and `/dev/fuse` is absent.

## Requested environment change

Run the development container with the namespace and FUSE permissions required
by Darling. A privileged container is the simplest known-good option. If a
minimal capability profile is preferred, it needs to permit Darling's UTS/IPC
namespace creation, mount/FUSE operations, and tracing, expose `/dev/fuse`, and
use a seccomp policy which allows those operations.

This is an environment limitation, not a LuaJIT runtime defect.

## Resolution

Resolved by the checked-in devcontainer launch profile, which runs privileged,
disables the seccomp and AppArmor filters, grants `SYS_ADMIN` and `SYS_PTRACE`,
and exposes `/dev/fuse`. After rebuilding, the current container has the needed
capabilities and device; `darling shell /usr/bin/true` exits successfully. The
remaining file-descriptor-limit warning is nonfatal and does not prevent the
macOS test runner from executing binaries.
