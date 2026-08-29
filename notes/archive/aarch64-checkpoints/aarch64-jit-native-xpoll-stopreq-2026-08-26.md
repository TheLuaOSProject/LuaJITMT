# ARM64 native XPOLL lifecycle proof (2026-08-26)

## Scope

This checkpoint executes the first admitted integer `BC_LOOP` through a
request which arrives after root-entry admission has completed but before the
ARM64 VM branches to trace machine code. It covers both TG-local profiling and
a fresh counted `STOPREQ` on ordinary macOS ARM64 and on ARM64e with BTI,
including the forced far exit-handler path.

It does not open another recorder, native-entry topology, IR shape, spill
shape, side trace, stitched trace, `JFUNCF`, or FFI trace call. The production
entry and XPOLL algorithms are unchanged; the new pause and exit observations
exist only in test-helper builds.

## Post-admission boundary

`lj_trace_enter_root()` now exposes
`LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION` under the existing ARM64 root-entry
test scaffold. Its source position is deliberately:

1. after the third and final `trace_root_entry_request_pending()` plus
   bytecode recheck;
2. before `setsbufL()`, result publication, return to the VM, and the VM's
   branch to mcode.

At this point `TG.jit_base` is already non-NULL and protects the validated
trace. A worker which waits for this exact stage and then publishes a request
cannot have that request rejected by the entry helper. The executable proof
requires zero entry cleanups and a native exit at the snapshot inherited by
`IR_XPOLL` from `IR_LOOP` (snapshot 5).

The CI contract also extracts the helper source and pins this ordering. It
requires exactly three request checks and orders the third check before the
post-admission pause, which must precede `setsbufL()`.

## Profile-only execution

The publisher changes only `TG.profile_request` from 0 to 1, matching the
TG-local SIGPROF publication surface. It does not change the handshake epoch,
pending count, reqmask, or poll word.

The first admitted trace entry exits as parent 1, snapshot 5. Snapshot restore
lands on the patched `JLOOP`, so the same Lua call then makes one normal native
re-entry and finishes through parent 1, snapshot 8. Consequently the exact
profile case is:

- two root publications and zero entry cleanups;
- two trace exits;
- first exit parent 1 / snapshot 5;
- last exit parent 1 / snapshot 8;
- result 210;
- `profile_request`, `reqmask`, `poll`, and `hs_pending` all zero afterward;
- global handshake epoch and TG acknowledgement epoch unchanged;
- `jit_base` NULL, and the original C frame and VM state restored;
- trace 1 still runnable with its exact admitted IR, snapshots, bytecode,
  mcode geometry, and direct or indirect exit-stub shape.

The existing last-exit counters could not prove the XPOLL exit because the
normal recovery exit overwrites them. Test-helper builds therefore also record
the first exit parent and snapshot, reset by
`lj_trace_test_reset_exit_stats()`. This preserves both observed exits instead
of weakening the assertion to the final snapshot.

## Counted STOPREQ execution

The publisher uses the same single-TG ordering already used by focused native
helper tests:

1. release-store global `hs_actions = LJ_GC2_HS_STOPREQ`;
2. release-store global `hs_pending = 1`;
3. release-store global `hs_epoch = old_epoch + 1`;
4. release-store TG `reqmask = LJ_GC2_HS_STOPREQ`;
5. release-store TG `poll = 1`;
6. release the paused root-entry helper.

This is intentionally a manual one-TG request, not a synchronous handshake
leader. It isolates owner-side native XPOLL, trace exit, acknowledgement, and
checked STOPREQ delivery without allowing a remote leader to consume the
request first.

The exact STOPREQ result is:

- one root publication and zero entry cleanups;
- one trace exit, parent 1 / snapshot 5;
- `lua_pcall()` returns `LUA_ERRRUN` with
  `thread interrupted: VM shutdown`;
- global epoch and TG acknowledgement both advance by exactly one;
- `hs_pending`, reqmask, poll, and profile request are zero;
- `TGF_STOPREQ_FRESH` is consumed by the checked VM landing;
- sticky `TGF_STOPREQ` is observed, then explicitly cleared together with the
  fresh bit before subsequent work;
- `jit_base` is NULL, and the original C frame and VM state are restored;
- the admitted trace remains runnable.

A final clean call resets the entry/exit counters, returns 210, and has exactly
one root publication, zero entry cleanups, and one parent 1 / snapshot 8 exit.
This proves the handled STOPREQ does not invalidate the trace.

## Validation

`tools/ci/arm64_jit_native_loop_contract.sh` passed end to end on the local
Apple Silicon host for:

- ordinary `-arch arm64` with the direct exit-handler branch;
- `-arch arm64e -mbranch-protection=bti` with the direct authenticated path;
- the same ARM64e/BTI fixture with `LUAJIT_MCODE_TEST=R`, forcing the far
  authenticated exit-handler path.

The contract's failure trap rebuilt the shared checkout in ordinary ARM64
experimental mode. `sh -n` and `git diff --check` also passed. The only build
warnings were the pre-existing unused `szmcode` local in `lj_trace.c` and
unused `ccall_rawchild_wait()` helper in `lj_ccall.c`.
