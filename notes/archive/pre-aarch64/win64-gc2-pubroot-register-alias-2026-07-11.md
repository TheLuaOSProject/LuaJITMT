# Win64 GC2 root-publication register alias (2026-07-11)

Status: fixed and validated against the current GC2-only branch. The plan files
were not edited.

The Windows UCRT build succeeded but faulted before the platform smoke could
print. The regression is exactly bracketed by `e565d8e8` (good) and
`edddec97` (bad), where `vm_gc2_pubroot` was introduced.

On Win64, DynASM assigns both `RA` and `CARG1` to `rcx`. The stub loaded
`SAVE_L` into `CARG1` and then evaluated `[RB+RA*8]`; the stack destination
index had therefore become the `lua_State *`. WineDbg observed the resulting
invalid TValue address in `rdx` at `lj_state_stack_pubtv`, with the caller
returning through `vm_gc2_pubroot`.

The destination address is now materialized in `CARG2` before `CARG1` is
loaded, and `L->base` is stored from the saved `RB`. A throwaway current-HEAD
UCRT build with this exact reorder exits zero under Wine and prints
`Windows x64`.

The release package has a separate deployment issue: the current UCRT DLL
imports `libwinpthread-1.dll`, but staging does not yet bundle that dependency.
The runtime proof copied the toolchain DLL beside `luajit.exe`; artifact staging
must be fixed before a Windows release candidate can be considered complete.
