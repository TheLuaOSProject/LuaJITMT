# Windows UCRT artifacts omit a required runtime DLL

## Impact

The current Windows x86-64 UCRT build succeeds, but its staged install tree is
not self-contained. Wine execution required manually copying
`libwinpthread-1.dll` beside `luajit.exe` and `lua51.dll`. A release archive made
by the current harness can therefore fail on a clean Windows/Wine environment.

## Evidence

`x86_64-w64-mingw32ucrt-objdump -p lua51.dll` lists:

```text
DLL Name: libwinpthread-1.dll
```

`tools/release/build_artifact.sh` stages `luajit.exe` and `lua51.dll`, but has no
rule for `libwinpthread-1.dll`. The complete Wine smoke only passed after the
DLL from the MinGW UCRT sysroot was placed alongside the binaries.

## Requested harness change

Choose and enforce one deployment policy:

1. bundle the matching `libwinpthread-1.dll` in the artifact and validate its
   checksum/origin; or
2. link the pthread runtime statically if its license and toolchain behavior are
   acceptable.

The release verifier should also run the archive in an isolated directory and
check non-system PE imports so a globally installed MinGW DLL cannot hide a
missing packaged dependency.

This is a release-harness/packaging defect, separate from the Win64 GC2 register
alias runtime bug fixed in commit `d2574282`.

## Resolution

Resolved in the release harness. The UCRT build now locates the matching
`libwinpthread-1.dll` through the selected cross compiler, bundles it beside the
Windows binaries, and records its absolute toolchain origin and SHA-256 in
`BUILDINFO`. Before testing or archiving, the builder rejects any non-system PE
import that is not present in that directory. Both the archive layout verifier
and the isolated archive smoke require the bundled DLL, so a host/Wine search
path can no longer hide its absence.
