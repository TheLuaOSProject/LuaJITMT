# Amalgamated VM resume-helper linkage (2026-07-14)

## Symptom

The final-candidate `m3_gc2_no_legacy_runtime` gate passed its ordinary GC2
artifact/runtime checks, then failed while linking `make -C src amalg`:

```text
lj_vm.o: undefined reference to `lj_state_resume_release_result'
```

All four references came from the x64 coroutine resume release/fallback paths.
The ordinary split-object build was unaffected.

## Root cause

Commit `f8ce402f` added `lj_state_resume_release_result()` as a C helper called
directly by the separately assembled VM. Its declaration used `LJ_FUNC`.
`LJ_FUNC` intentionally becomes `static` when compiling `ljamalg.c`, so the C
compiler saw no in-translation-unit caller and discarded the helper. `lj_vm.o`
still required the external symbol at final link time.

This is an artifact-linkage bug, not a GC protocol or coroutine ABI change.
It predates the `b86b3433` embedded-empty-string checkpoint and was exposed by
the first final-candidate amalgamation gate.

## Fix

The declaration now uses `LJ_FUNCA`, the existing annotation for helpers called
from generated/assembled VM code. This keeps the symbol externally linkable in
the amalgamated object. Split-object visibility and the helper's C signature,
calling convention, behavior, and public LuaJIT API/ABI are unchanged.

The change adds no lock, wait, collector fallback, or runtime instruction.
`plan/` remains unchanged.

## Validation

- `make -C src clean && make -C src -j8 amalg`: passed.
- `m3_gc2_no_legacy_runtime`: passed for both split and amalgamated artifacts;
  the retired-symbol scan passed and both binaries completed repeated GC2
  cycles plus active-SWEEP `lua_close()`.
- `m4_threading_coroutine`: passed.
- `m4_threading_coroutine_joff`: passed.
- `m5_state_owner`: passed its native owner/claim fixture.

The suite restores a clean normal build after its amalgamation profile, and
that restoration also passed.
