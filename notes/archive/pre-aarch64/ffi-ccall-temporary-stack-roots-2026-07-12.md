# Interpreted FFI C-call temporary stack roots (2026-07-12)

## Problem

Several target ABIs require the interpreted FFI caller to materialize a
by-value aggregate in caller-owned storage and pass a pointer to it. The
Windows x64 struct and complex argument macros, and the corresponding ARM64
and PPC cases, allocated that storage as cdata but kept only `rp` and the native
argument pointer. Later CType snapshot waits, argument conversions, native
entry, callbacks, and complete GC2 cycles could therefore retire or reuse the
cdata while the foreign function still owned the pointer.

Aggregate results had a related lifetime requirement. `ccall_set_args()`
preallocates their cdata before argument conversion and hands its payload to
the ABI as the hidden result buffer. The object must remain live through every
later conversion, the complete native call (including callbacks), the final
CType snapshot, and result extraction.

Neither pending ownership nor a C local is a semantic GC2 root. Adding a TG
anchor would also be the wrong error contract here: a callback error unwinds
the Lua call frame directly, so an out-of-frame anchor could leak unless every
callback escape knew how to repair it.

## Stack-root protocol

`lj_ccall_func()` now pre-grows its Lua frame before constructing any ABI
temporary. Each cdata is copied to `L->top`, release-published through
`lj_state_stack_pubtv()`, and only then exposed as a native pointer. These are
ordinary frame roots, so normal return and every parser/conversion/callback
error use the VM's existing stack unwind.

The aggregate-result root is always the first appended slot. Argument roots
follow it in conversion order. They stay live through `lj_ccall_native_leave()`
and the post-call CType snapshot. The caller then restores the saved top,
retaining only that first result slot when one exists. This preserves the
historical `ccall_get_results()` top-minus-one contract and prevents temporary
roots from changing the Lua-visible result count or value.

After result conversion, the final result slot is published uniformly before
STOPREQ handling and allocation-driven GC checks. This also closes scalar
cdata results created directly in `L->top-1`; the earlier aggregate root was
already published.

The pre-growth bound is exact and architecture-derived, not signature-shaped:

- every by-reference temporary consumes either one argument GPR position or
  one `CCallState.stack` pointer slot;
- FPR-only arguments do not create these temporaries;
- `CCALL_NUM_GPR + CCALL_NUM_STACK` therefore bounds all successfully placed
  argument temporaries on the supported ABIs;
- one additional argument slot covers the single over-limit temporary which a
  target macro can allocate immediately before ABI placement detects stack
  exhaustion and raises the existing NYI-call error; and
- one final slot holds the optional aggregate result.

The reservation is capped first by the actual Lua argument count, so a small
call does not grow its stack for the maximum ABI call-state footprint. On
targets whose argument macros never allocate cdata temporaries, including
x86-64 SysV on Linux and macOS, the argument-root bound is compile-time zero;
those targets reserve only the possible aggregate-result slot. This avoids an
unnecessary stack growth or near-limit rejection without inspecting the
signature. The common path adds one bounded stack-capacity check.
Temporary-root publication work is paid only for actual aggregate roots; there
is no lock, global registry, signature dispatcher, or explicit C-call shape
match.

## Adjacent CLibrary symbol handoff

`lj_clib_index()` previously created symbol cdata in a C local and then called
the throwing stack-grow helper before copying it to the cache-publication
anchor. It now reserves the slot before cdata construction and release-publishes
the copied anchor before either cache table can allocate, wait, or start a GC2
cycle. Constant symbols use the same harmless publication path.

## Deterministic coverage

`tests/t-ffi-ccall-temp-roots-lib.c` exports one generic ABI function with a
64-byte by-value input, a Lua callback, and a 64-byte aggregate result. The
function invokes Lua first and performs volatile input loads only after the
callback returns. The callback runs repeated complete collections and creates
1024 same-sized cdata objects to force reuse pressure. The Lua fixture verifies
every input/result word, exact one-result shape, callback-error `pcall` unwind,
and a successful collected call after that unwind.

The same source has useful platform-specific force:

- Linux and macOS x86_64 exercise the hidden caller-owned aggregate result
  buffer across callback GC. Their SysV ABI passes the large input directly on
  the native stack, as expected.
- Windows x64 also passes the large by-value input through the exact
  `CCALL_HANDLE_STRUCTARG` cdata temporary. Cross-compiled assembly retains its
  input pointer from `RDX`, invokes the callback from `R8`, and reloads all
  input words through that pointer afterward, so the test cannot succeed by
  hoisting the reads before collection.

Verification completed for this slice:

- native Linux `m7_ffi_ccall_native`, including the new fixture;
- native Linux `m7_ffi_clib_cache` in interpreter and JIT modes;
- x86_64 Windows UCRT cross-build plus the focused fixture under Wine 10; and
- x86_64 macOS 13 cross-build plus the focused fixture under Darling.

All four focused runs passed. A Win64 negative-control build which reverted
only the two argument-temporary root calls failed deterministically on round
one, lane zero after the callback collections (observed `5909105`, expected
`5921221`). The fixture therefore detects the exact lifetime gap rather than
merely exercising the ABI.

This is a lifetime/publication tranche, not a claim that the remaining FFI,
GC2, JIT, or release gates are complete.
