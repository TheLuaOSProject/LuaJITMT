# Cleanup Dedup Pass - 2026-07-05

This pass kept behavior unchanged and removed repeated fork-local scaffolding.

## Closure Bump Allocation

`lj_func.c` had the same allocator-readiness predicate in the closed-upvalue,
one-upvalue closure, and no-upvalue closure bump paths. It now uses a single
local `func_bump_alloc_ready()` helper. The helper documents why these fast
paths are only valid for the main-TG, arena-backed, no-MT/no-worker window.

The FNEW bump fixture now covers the predicate directly:

- `mt_entering` blocks the one-upvalue, upvalue-cell, and no-upvalue bump paths.
- registered GC2 workers block one-upvalue bump allocation.
- disabling the arena allocator shim blocks one-upvalue bump allocation.

## Safepoint Wrappers

Several native-call sites repeated identical `had_stopreq`/fresh-stop wrappers.
`lj_safepoint_had_stopreq()` now lives next to the existing fresh-stop helpers,
and exact duplicate local wrappers were removed from base `print`, `loadfile`,
package loader paths, `jit.profile.stop`, FFI string/copy/fill helpers,
VM-event failure reporting, JIT-token waits, the FFI CLibrary loader,
`debug.debug` native console I/O, FFI native-call helpers, profiling stop, and
the CLI frontend, I/O library native stdio wrappers, channel parks, and FFI
callback native boundaries. Threading join/spawn/mutex/sleep waits now use the
same shared helpers while preserving the explicit join pre-poll. CType parser
token waits and secure-PRNG native entropy reads now use those shared nullable
helpers too. Table resize/retry waits and OS-library native wrappers also share
the common had/fresh helper path; OS keeps only its explicit pending-STOPREQ
pre-poll because successful `os.tmpname()` must remove a just-created temporary
file before throwing.

Site-specific wrappers were intentionally left in place when they carry behavior
beyond the common helper, such as extra pending-poll handling or additional
cleanup before throwing a fresh STOPREQ.

## Table Retire Helpers

`lj_tab` node and array retirement records share the same Treiber-list metadata:
retired head, next link, retire epoch, and armed bit. The typed accessors and
push loops now use local macro generators for those identical parts, while the
payload fields, free checks, and reclaim loops stay explicit for node and array
ownership semantics.

## FFI Parser-Token Fixtures

Several M7 FFI snapshot fixtures repeated the same parser-token release worker:
hold `CTState.parse_token`, wait until the tested thread reaches native wait,
release the token, then assert the sequence advanced. The shared
`ctype_parse_fixture_helpers.h` helper now owns that pattern, so snapshot tests
keep the same nonblocking wait assertion without local copies.

The follow-up pass routed the remaining exact-match metadata, conversion,
namespace, metatable, arithmetic, and ccall snapshot fixtures through the same
helper. CDEF-token, callback, native-stopreq, and parser-release-only tests keep
local workers because they assert different cleanup or observation semantics.

## M6 Build Profile Cleans

`m6_jit_table_store_helper` and the M6 TBAR paranoia sub-check no longer call
`make clean` immediately before a profile-aware harness build. `Test:build()`
already cleans when the requested `XCFLAGS` signature differs, and
`with_default_build_restore()` still restores the default profile after the
paranoia sub-check. Removing the extra clean keeps the isolation semantics while
avoiding duplicate full source-tree rebuilds.
