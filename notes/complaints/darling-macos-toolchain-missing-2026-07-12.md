# Darling image-local toolchain discovery was misleading (2026-07-12)

Status: resolved; this was a harness-discovery error, not a missing capability.

Darling itself starts, so runtime testing of a Mach-O binary is available.  A
first inspection looked only inside the Darling prefix and incorrectly
concluded that the environment could not build a new macOS binary:

- `xcrun` resolves its developer directory to
  `/Library/Developer/DarlingCLT`, but that tree has no `usr/bin/clang`;
- the image has no CommandLineTools SDK directory; and
- ordinary macOS libc headers such as `/usr/include/stdio.h`, `stdlib.h`, and
  `sys/types.h` are absent.

Observed commands:

```text
$ darling shell ls -ld /Library/Developer/DarlingCLT/usr/bin/clang
ls: /Library/Developer/DarlingCLT/usr/bin/clang: No such file or directory

$ darling shell ls -l /usr/include/stdio.h /usr/include/stdlib.h /usr/include/sys/types.h
ls: ... No such file or directory
```

The repository already contains a complete host-side osxcross installation:

```text
.devcontainer/osxcross/target/bin/o64-clang
.devcontainer/osxcross/target/bin/x86_64-apple-darwin23.2-clang
.devcontainer/osxcross/target/SDK/MacOSX14.2.sdk
```

It was simply not on the shell's default `PATH`.  macOS validation should add
`.devcontainer/osxcross/target/bin` to `PATH`, cross-compile the x86-64 Mach-O
artifacts with osxcross, and execute those artifacts under Darling.  No user or
harness action is required for this item.
