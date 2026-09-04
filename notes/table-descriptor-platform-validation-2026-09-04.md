# Descriptor installation platform validation

Date: 2026-09-04

The macOS x86-64 descriptor fixture passes under Darling. The Windows x86-64
UCRT build and the two new installation regressions pass under Wine, but the
full Windows fixture fails an existing requirement that a pre-MT table-store
loop produce a trace. An exact HEAD runtime/fixture comparison reproduces that
same failure. **This is not a full Windows fixture pass or proof of Windows
JIT table-store support.**

## Source and build isolation

Base commit: `a649f737d9841e1bf17f9102fb526d6bfb6c29e3`.

Workspace-local build products and unrelated current edits were excluded.
`git archive HEAD` was extracted into two independent trees, then only root's
current `src/lj_tab.c` and `tests/t-tab-resize-descriptor.c` were copied over the
archive. No runtime/test source edits or builds were performed in the shared
tree by the platform reviewer.

Artifacts and logs are in:

```text
/tmp/lj-tabdesc-platform-20260904-awxurb1q/
```

The exact overlaid source SHA-256 values are:

```text
ee0a474944854a2d6ccd63cb1f885abb8975044f8392dc93e695465068877c2f  src/lj_tab.c
f61d0849c375340c1f105b346e8287d0284477900a28283c6eed5aa7415fe47a  tests/t-tab-resize-descriptor.c
```

Builds were restricted to CPUs 0-3 for macOS and 4-7 for Windows, avoiding the
performance agent's CPU 30. Both builds used
`-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT`.

## macOS / Darling

Toolchain: repository osxcross, Debian Clang 19.1.7 targeting
`x86_64-apple-darwin23.2`. Deployment target: macOS 11.0.

The exact build command was:

```sh
MACOSX_DEPLOYMENT_TARGET=11.0 \
PATH=/workspaces/lj-lockless/.devcontainer/osxcross/target/bin:$PATH \
taskset -c 0-3 make -j4 \
  -C /tmp/lj-tabdesc-platform-20260904-awxurb1q/macos \
  HOST_CC=gcc CC=clang CROSS=x86_64-apple-darwin23.2- \
  TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64' BUILDMODE=static \
  XCFLAGS='-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT'
```

It exited 0. The build log contains no compiler warnings or errors.

Fixture compile/link command:

```sh
MACOSX_DEPLOYMENT_TARGET=11.0 taskset -c 0-3 \
  /workspaces/lj-lockless/.devcontainer/osxcross/target/bin/o64-clang \
  -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 \
  -DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT \
  -I/tmp/lj-tabdesc-platform-20260904-awxurb1q/macos/src \
  /tmp/lj-tabdesc-platform-20260904-awxurb1q/macos/tests/t-tab-resize-descriptor.c \
  /tmp/lj-tabdesc-platform-20260904-awxurb1q/macos/src/libluajit.a \
  -lm -pthread \
  -o /tmp/lj-tabdesc-platform-20260904-awxurb1q/tabdesc-macos
```

Compile/link exited 0. `file` identifies a Mach-O 64-bit x86_64 executable.

Runtime command:

```sh
DPREFIX=/darling/prefix taskset -c 0-3 timeout 180s \
  darling shell \
  /Volumes/SystemRoot/tmp/lj-tabdesc-platform-20260904-awxurb1q/tabdesc-macos
```

Exit status: **0**. Runtime output:

```text
Warning: failed to increase FD rlimit: Operation not permitted
t-tab-resize-descriptor OK: persistent ids survive GC and enter SMR retirement
```

This is the complete unchanged overlaid fixture, including both new install
regressions and the existing pre-MT trace flush, VM store, GC, reclamation, and
shutdown cases.

## Windows / Wine

Toolchain: `x86_64-w64-mingw32ucrt-gcc (GCC) 14`.
Runtime: `wine-10.0 (Debian 10.0~repack-6)` with the existing `.release-wine`
prefix.

Production build command:

```sh
taskset -c 4-7 make -j4 \
  -C /tmp/lj-tabdesc-platform-20260904-awxurb1q/windows \
  HOST_CC=gcc CC=gcc CROSS=x86_64-w64-mingw32ucrt- TARGET_SYS=Windows \
  XCFLAGS='-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT'
```

It exited 0. GCC reported three unused-static-function warnings already
present in these sources: `asm_ahstore_forjit`, `emit_lockrmro`, and
`ccall_rawchild_wait`. The production build was not claimed as `-Werror` clean.

The fixture linked statically against all 77 production `.o` files except
`luajit.o`, following `tools/ci/build_windows_gc2_cell_fixture.sh`. The actual
orchestration built the object argument list with Python; this Bash command is
the equivalent exact compiler invocation:

```sh
build=/tmp/lj-tabdesc-platform-20260904-awxurb1q/windows
objects=()
for object in "$build"/src/*.o; do
  case "${object##*/}" in
    luajit.o) ;;
    *) objects+=("$object") ;;
  esac
done
taskset -c 4-7 x86_64-w64-mingw32ucrt-gcc \
  -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 -static -static-libgcc \
  -DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT \
  -I"$build/src" "$build/tests/t-tab-resize-descriptor.c" \
  "${objects[@]}" -lm -lsynchronization \
  -o /tmp/lj-tabdesc-platform-20260904-awxurb1q/tabdesc-windows.exe
```

Strict fixture compile/link exited 0. `file` identifies a PE32+ x86-64
executable. Import inspection shows only `KERNEL32.dll`, Windows API-set DLLs,
and UCRT API-set DLLs; it does not depend on an unstaged MinGW runtime DLL.

Runtime command:

```sh
WINEPREFIX=/workspaces/lj-lockless/.release-wine WINEDEBUG=-all \
taskset -c 4-7 timeout 180s \
  wine /tmp/lj-tabdesc-platform-20260904-awxurb1q/tabdesc-windows.exe
```

Exit status: **3**. Output:

```text
error: XDG_RUNTIME_DIR is invalid or not set in the environment.
Assertion failed: lj_trace_hasany(g), file /tmp/lj-tabdesc-platform-20260904-awxurb1q/windows/tests/t-tab-resize-descriptor.c, line 1003
```

The assertion is in `exercise_private_store_gates()`. The new installation
cases occur earlier and execute successfully, but this failure prevents the
full fixture from reaching its final success message.

### Baseline comparison

The built Windows tree was copied to `windows-head`, both overlaid files were
replaced from the original `head.tar`, `src/lj_tab.c` was touched to force its
recompilation, and the same production build command was rerun with that tree.
The rebuild log confirms `CC lj_tab.o`, DLL relink, and executable relink.
The original HEAD fixture then linked against those baseline objects with the
same strict flags as above.

`tabdesc-windows-head.exe` under the same Wine command exits **3** with:

```text
error: XDG_RUNTIME_DIR is invalid or not set in the environment.
Assertion failed: lj_trace_hasany(g), file /tmp/lj-tabdesc-platform-20260904-awxurb1q/windows-head/tests/t-tab-resize-descriptor.c, line 884
```

The line moved only because the patched fixture adds the two new cases. This
runtime comparison establishes the full-fixture failure exists at exact HEAD,
before the paired installation change.

### Isolated new regressions and diagnostic evidence

A separate temporary fixture copy retained the normal initial tests and added
an early successful exit immediately after
`exercise_install_competing_generation()` and `exercise_install_weak_update()`.
It did not remove or change either new test's assertions. This temporary copy
compiled with the same `-Wall -Wextra -Werror` flags, linked against the patched
Windows production objects, and ran as
`tabdesc-windows-install-regressions.exe` under the same Wine command.

Exit status: **0**. Output:

```text
error: XDG_RUNTIME_DIR is invalid or not set in the environment.
isolated new descriptor installation regressions OK
```

A second temporary copy added a `jit.attach(..., 'trace')` diagnostic and
printed `jit.status()` around the original store loop. JIT was enabled, but
the loop repeatedly emitted trace abort reason **7** (`LJ_TRERR_NYIBC`). The
diagnostic did not skip the trace assertion; it also exited 3.

The corresponding source restriction is explicit:

- `src/lj_record.c:44` enables `LJ_HAS_X64_MT_JIT_HELPERS` for Linux/macOS only.
- `src/lj_record.c:2583` rejects HSTORE and ASTORE with `LJ_TRERR_NYIBC` when
  that helper macro is disabled, including the pre-MT fixture loop on Windows.

This restriction is an outstanding Windows runtime capability issue. No shared
fixture assertion was weakened and no Windows runtime fix was made as part of
this validation.

## Retained logs

All names below are relative to the artifact directory recorded above:

```text
source-head.txt
macos-build.log
macos-fixture-build.log
macos-fixture-run.log
windows-build.log
windows-fixture-build.log
windows-fixture-run.log
windows-head-build.log
windows-head-fixture-build.log
windows-head-fixture-run.log
windows-install-regressions-build.log
windows-install-regressions-run.log
windows-diag-run.log
```
