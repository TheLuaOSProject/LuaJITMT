# Generated callback stack geometry, 2026-09-05

Generated foreign-call entry now installs its validated XSAVE base/top in the
owning Lua state before publishing native execution. This repairs a real
first-callback stack overwrite: a warmed scalar loop could return 4 instead
of 6640, or trap on a corrupted numeric-for argument in assertion builds.

The original hardware watchpoint found actual owner base/top at stack slots
25/29 while the retained generated frame required 37/40. Callback continuation
setup used the stale top and overwrote the live loop step at slot 32. The
post-call snapshot correctly omitted that unchanged step. The permanent
regression uses scalar arithmetic and an ordinary
`int(*)(int(*)(int), int)` callback ABI, removing the original struct payload.

The two pointer stores follow owner/carrier, stack geometry, trace identity,
pin and successful frame-push checks. Rejected entry leaves the old owner
pointers untouched. No allocation, relocation or callback occurs between
validation and publication. Native release ordering makes the owner writes
visible before certified remote native reads. Publishing only during callback
setup would be too late for its preceding owner-root safepoint.

Nested generated calls still push above suspended frames, and callback stack
growth still uses retained offsets. Existing interpreter calls and foreign
auto-attachment use their own entry routes. This adds no lock, wait, or new
admission path; separate callback-carrier and native-acknowledgement progress
dependencies remain. No timing claim is made for the two entry stores.

The production patch was isolated against `b4e26564`. Only `src/lj_ccall.c`
changes in that runtime; its final SHA-256 is
`4c3668a5da4d83b7987dfb62e92344ad34264274e5272bad4c7cab33d61d3f07`.
The exact source/test patch is
`bc8301d1ef7e8475520cacaddc734d418185b2800237c4c3e713c98bd14d0580`.
[The implementation handoff](evidence/ffi-callback-stack-geometry-2026-09-05/isolated/HANDOFF.md)
and [independent review](evidence/ffi-callback-stack-geometry-2026-09-05/independent/review.md)
retain the publication, suspended-root, nested-frame and unwind arguments.

The permanent `t-ffi-callxs-callback-stack.c` fixture and canonical
`m7_ffi_callxs_callback_stack` case verify:

- 80 real generated callbacks preserve the exact scalar-loop result.
- A callback actually relocates its stack with `lua_checkstack(2048)`, runs
  full collection while the generated frame is suspended, and resumes with
  the correct result.
- A throwing callback has one generated entry, one callback and no generated
  return; native/callback depth, mirrors and trace pins are clean after unwind.

Exact baseline normal and assertion runtimes fail the unchanged scalar
regression. Candidate normal, assertion and Clang ASan/LSan runtimes pass all
three modes. Existing callback-runtime, foreign-call and native-frame
canonical cases pass. Standalone authentic generated nesting and post-call
forced-exit/STOPREQ fixtures pass with matching helper-enabled builds.

Final integration combines this patch with the committed mode-0 poll repair
`8d342cd6`. All 793 source/test inputs match across normal, assertion and ASan
trees. All 31 executed test processes pass: stock tests report 387 with JIT
off and 509 with JIT on in each tree; cdata method guards, interpreter capture,
both first-attachment modes and the new callback regression pass. Matching
assertion and ASan runtimes also pass the existing nested callback and
post-call fixtures. The helper-free normal build refuses compilation of the
older callback fixture that explicitly requires `LJ_XSAVE_TEST_HELPERS`;
that setup error is preserved and is not an executed test. The shared default
build passes the new canonical case using a private temporary directory.

ASan instruments runtime targets, with host generators checked separately as
uninstrumented. Runtime options are `detect_leaks=1:abort_on_error=1`, without
suppressions. The separate pure-cdata load optimization is not included.

Two older broad gates still fail on both exact baseline and candidate:
`t-jit-xsave.c:976` finds its staging poison unchanged, and the remote-flush
Lua fixture receives no worker-ready message at line 220 within its own bound.
Their failures and baseline comparisons remain recorded. The authentic
aggregate stops early, so its later subtests are not claimed as aggregate
passes; relevant nested callback and post-call fixtures ran separately. The
XSAVE fixture's later synthetic admission assertions likewise do not supply
passing coverage here. Follow-up diagnosis remains separate from this repair.

[The evidence manifest](evidence/ffi-callback-stack-geometry-2026-09-05/artifact-manifest.json)
retains exact commands, source and binary identities, the original watchpoint,
earlier fixture corrections, baseline failures, independent review and final
combined results. The implementation bundle's 128 named text artifacts were
hash-verified before integration. Validation is Linux x64; Windows/macOS work
remains deferred until preparation for the next release.
