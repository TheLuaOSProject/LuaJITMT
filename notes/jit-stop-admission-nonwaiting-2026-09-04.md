# Nonwaiting admission for side-trace publication

Date: 2026-09-04. Stability is the acceptance criterion for this change; no
performance improvement is claimed.

The isolated validation baseline is
`90db531bf837fc7674342c233c43130e455bfffd`, with this change overlaid in
`src/lj_trace.c` and its new fixture. This is a bounded publication fix, not
completion of the JIT, GC, FFI, event, or platform progress work.

## Publication and lifetime contract

`trace_stop_admit()` performs one `lj_gc2_smr_read_try()` before committing
machine code or publishing the final trace. It validates the exact runnable
parent and root, plus the side exit's snapshot bounds and exit-table presence.
Failure raises ordinary `LJ_TRERR_RETRY`; all reader counts are released before
raising an error. The existing speculative abort path then discards the pending
slot, retires the compact assembler scratch, releases its exit-table ownership,
and publishes IDLE before releasing the recorder token.

Successful admission hands exact live-body lifetime to the recorder token that
this caller already owns. That proof is stronger than merely caching a pointer
observed under an expired reader:

- `lj_trace_retire_gc_claim()` takes that same token before the retirement
  epoch claim, native-pin closure, retire-list publication and disconnection.
  A peer that loses the token requests recorder abort and returns.
- `trace_scope_clear_slot()`, `trace_slot_retire()` and
  `trace_retired_slot_release()` require recorder-token ownership.
- `lj_trace_reclaim_retired()` also takes the token opportunistically before
  detaching lists or physically destroying trace bodies and exit tables.
- Initial validation rejects retired or entry-gated bodies. The VM's
  token-free optimized-next invalidation may close native entry, but it cannot
  disconnect the graph or destroy these bodies without the subsequent token
  transaction.

Consequently, a peer can close metadata SMR after admission, but cannot retire
the admitted live parent/root while this recorder still owns the token. The
post-save side/stitch graph updates use these exact bodies directly; their
former second looping reader admissions are removed. Side metadata is still
published before the release store to the parent exit target. Root bytecode
publication retains its existing CAS and immutable recovery sidecar.

Same-owner collection is a separate concern: a token does not exclude its own
owner. The core between admission and link publication therefore contains no
trace-retirement or arbitrary Lua callback boundary:

- `lj_mcode_commit()` changes the reservation/protection state.
- Linux `lj_mcode_sync_core()` may enter native state and acknowledge a
  safepoint. Owner ACK can request asynchronous recorder abort, publish roots,
  rotate SSB work, and update allocation accounting/dispatch. It does not run
  trace-slot retirement or release the recorder token. Profile owner polling
  only installs the pending hook; it does not call the profiler callback.
- `trace_save()` now initializes the compact body, transfers constructor and
  exit-table ownership, publishes the slot, and issues `lj_gc_pubtrace()`.
  The fresh-root publication and marking/SSB conversion paths do not run the
  trace-pruning/destruction transaction.
- Optional debugger registration and perf-map I/O follow all parent/root uses
  and runnable graph publication. Debugger allocation may account enough bytes
  to assist GC under the same owner, so retaining it between admission and
  parent linking would invalidate the token-only lifetime argument.

No SMR reader is held through native synchronization, debugger allocation,
perf-map I/O or a user event. The source retains the existing optional-metadata
ordering relative to STOP/ABORT delivery. Optional debugger metadata can now
appear after runnable entry becomes visible; it remains optional diagnostic
metadata, not the runtime's machine-code execution or exception-unwind gate.

## Errors and event semantics

After `trace_save()`, `J->curfinal`, `J->cur.traceno`, and the current scratch
exit-table pointers are cleared; their ownership belongs to the published
trace. If optional metadata allocation throws, `lj_trace_ins()` propagates the
external error after `lj_trace_abort_owner()` clears only remaining unpublished
state. It does not retire or clear the already linked trace. GDB allocates its
descriptor before storing `T->gdbjit_entry`, so failure at that allocation leaves
no partial descriptor. The fixture checks this exact allocation-call boundary
and later executes the function successfully.

The change uses ordinary RETRY and does not expand `SMRRETRY`'s special event
suppression. On validation failure with event admission open, the ordinary
abort callback can still inspect the recorder IR before its pending slot is
cleared. If the real reclaimer remains exclusively closed, existing legacy
VM-event preparation can return its ordinary one-shot contention result. No
event API, argument layout, reentrant-control behavior or handler-error policy
is replaced here. Token-held START/STOP/ABORT/RECORD callbacks remain separate
progress debt.

## Deterministic evidence

`tests/t-jit-stop-admission.c`, registered as `m6_jit_stop_admission`, compiles a
real root trace and makes a different branch hot to reach authentic
`BC_JMP` side publication. It uses the existing real IDLE reclaimer pause after
native quiescence, not a mocked SMR return value. The mutator completes and
releases the recorder token before the fixture permits the reclaimer to resume.

| Schedule | Required observed outcome |
| --- | --- |
| Reclaimer closes before admission | Lua returns its correct result; no child slot or parent link is published; mcode cursor is unchanged; scratch is listed as unpublished retirement; recorder and SMR counts are clean. |
| Reclaimer closes after admission, before save | The child and parent exit link are fully published while the reclaimer remains paused. |
| Reclaimer closes after save, before parent linking | The same complete publication occurs without a second SMR admission. |
| Parent invalidates before admission with SMR open | The pending attempt aborts, its parent metadata remains unchanged, and exactly one ordinary RETRY callback sees inspectable recorder IR. |
| Optional GDB allocation fails after publication | The actual `gdbjit_newentry()` call to `lj_mem_realloc` is intercepted with GNU ld `--wrap`; it raises `LUA_ERRMEM`. The child remains linked and executable, the GDB entry is null, and recorder/scratch/SMR cleanup completes. |

The first four cases pass with assertions and GC2 test helpers. All five pass
with both `LUAJIT_USE_GDBJIT` and `LUAJIT_USE_PERFTOOLS` enabled.
The registered `sh tools/ci/lua_test.sh m6_jit_stop_admission` case also passed
end to end in the isolated tree, including both clean fixture builds and its
default-build restoration (44.625 seconds).

Two isolated negative controls reintroduced a looping admission: first at the
initial lookup, then between save and parent linking. Both were stopped by the
fixture's deliberate 15-second alarm with SIGALRM (exit `-14`), after
15.002399 and 15.006849 seconds respectively. These are expected detection of
the forbidden wait, not passing production runs or unexplained timeouts. The
correct isolated source and library were restored afterwards.

Additional successful focused checks:

- Recorder-token and immutable-start-instruction fixtures; the latter also
  passed in the combined optional build and covers abort-event inspection.
- Real perf-map native STOPREQ fixture, including error propagation and close.
- VM-event flush and reentrant event parking. The flush test's intentional
  handler-error diagnostic is expected and was retained in its stderr log.
- Trace-pressure GC behavior in the normal and optional builds.
- Existing GDB publication/republication and concurrent-publication workloads.
- Stock `misc/jit_flush.lua` and authentic production CALLXS tests, including
  their exact foreign-effect counts and no-replay assertions.

Direct isolated builds used GCC 14.2.0, default Linux x64 optimization options, `CCDEBUG=-g`,
`TARGET_STRIP=:`, `-DLJ_GC2_TEST_HELPERS -DLUA_USE_ASSERT`, and the optional
flags above where stated. Direct focused test children were pinned to CPU 30;
the registered suite used its normal build defaults and available affinity.
Source trees,
build logs, stdout/stderr and process records are under
`/tmp/lj-jit-stop-admission-2026-09-04` and
`/tmp/lj-jit-stop-admission-optional-2026-09-04`. No shared workspace runtime was
built; no GC source was edited for this change.

## Explicit remaining scope

Production stitching is disabled at this baseline: `lj_trace_stitch_probe()`
returns zero and `lj_trace_stitch()` returns immediately. The CALL/CALLM/ITERC
publication arms share the new admission helper, but this fixture establishes
ordinary side-trace coverage only. Re-enabling stitching requires its complete
caller, snapshot, native-return and event proofs; it is not part of this change.

The root bytecode-CAS-loser path still performs a looping retirement admission
after its observable abort callback. At that point the body is already
published; speculative abort is not a sufficient replacement for the required
durable retirement work. Public/automatic flushing and other JIT lifecycle
waits likewise remain. This fixture is Linux-specific and establishes no fresh
Windows or macOS result.
