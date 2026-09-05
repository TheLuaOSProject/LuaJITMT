V1 accepts the bounded scalar-array iterator repair on exact 79345529 sources.
The unchanged original IDLE-entry regression passes in all three matching
source builds, with its original paused-window ITERN and later outcomes intact.
There are 123 final successful runtime processes. This is a progress/correctness
result, not a benchmark or a claim that every table operation is nonwaiting.

The production delta is still candidate-v1.patch, SHA256
fe8b4e4598f56e420c302ccd4545fd90c2707d59688565943b69044e7fb482f8.
Only lj_tab.c and the LJ_TAB_TEST_HELPERS declaration in lj_tab.h differ from
79345529. ROOT reviewed this exact patch before validation. All 224 runtime
and generator inputs are byte-identical between candidate (assert/APICHECK),
optimized (GC2 helper only), and ASan (same eight helper/assert flags as
candidate). There is no worker/fairness/current-HEAD combination in this study.
The earlier source/build review manifest remains unchanged and correctly records
zero runtime passes *at that earlier stage*; validation-summary.json owns the
subsequent runtime evidence.

Final validation comprises:

- 3 exact original t-jit-idle-reclaim-entry processes, original 20 second limit.
- 72 real-paused-writer progress processes: VM ordinary next, witnessed
  BC_ISNEXT/BC_ITERN, direct rooted next with input/output alias, and direct
  internal cursor; dense, sparse separate array, empty/null array, all holes,
  numeric zero/-0, and booleans. Each checks results before reclaimer release,
  IDLE META_EXCLUSIVE state, zero SMR readers, closed native entry, and unchanged
  root anchors. Helper builds additionally require no table wait calls.
- 22 authority processes, eleven modes each in assert and ASan: scalar values
  including NaN/infinities/-0; cursor validation and beyond-end KEYINDEX; every
  distinct pair of the four source/output slots plus local outputs; opaque GC
  and internal words; protected candidate/key/result/vector addresses;
  registered-but-unallocated protected bodies; malformed vector/colo extents;
  all source/owner stages for FOUND and END; real resize/full collection; and a
  real unrelated plain-arena writer that must cause bounded refusal.
- 12 stack-retry processes in assert and ASan: the established retry hook
  closes all authority, actually resizes, moves the stack and fully collects,
  then pauses a newly admitted IDLE metadata writer. The next attempt has one
  observed scalar-source/result visit and publishes into rebased destinations.
  FOUND and END each cover distinct outputs, key alias, and both source aliases.
  END preserves output words and cursor. The pending allocating retry hook is
  not stolen by the scalar primitive. Exactly the one established outer wait
  occurs; the new scalar attempt adds none.
- 4 lifetime/preflight processes in assert and ASan: null arguments, wrong
  owner word, a genuinely foreign physical thread, forwarded source word, and
  a real separate vector first retired by resize and then physically freed by
  full collection. The latter checks the actual old block bit is zero before
  reinstalling its address temporarily for a refusal probe; no freed header is
  read. The current vector keeps the same arena mapping live.
- 4 unchanged exact793 C regressions: complete scalar-hit and rooted-reader
  suites in assert and ASan, including the general GC/hash/API paths under
  their original open-writer schedules.
- 6 stock processes, off/on in all three builds; 387/509 stock cases pass per
  build. The stock input tree and both C regressions are exact793 git blobs.

The new source-stage hooks perform only bounded reads/assertions and changes to
already owned words. They do not allocate, throw, move the stack, wait, or call
Lua. Allocating resize/GC/stack movement happens between attempts through the
existing rooted retry hook, after all leases and SMR readers are gone. Every
non-success primitive result checks bit-exact unchanged output words/cursor,
unchanged stack dirty epoch, root anchors, SMR readers, wait counters, GC total,
and relevant mark words. Relevant arena publisher words are unchanged and
their low-bit reader counts are zero. Successful scalar publication retains the
ordinary stack dirty/release/barrier convention.

Lifetime scope is explicit. Protected addresses and real retire/free controls
exercise admission before payload; source/owner/vector mutations exercise
post-copy validation; all alias stores follow final source reads. Actual
collection is before/after admitted scalar attempts and in the released retry
window. These are not a claim of arbitrary simultaneous structural mutation or
full GC overlap at every instruction. Small/body/vector allocator gates and
all OPEN-path/general-reader gates remain intact. Plain-arena writer, Huge,
custom allocator, non-nil hash node, GC/internal result, and invalid source
cases refuse. A refused result is never fabricated as nil or END.

Preserved failures and controls matter:

- Exact793 baseline VM next+dense and direct rooted+empty each hit SIGALRM at
  four seconds while the real IDLE writer remained paused. V1 passes those
  exact fixture bytes and arguments. The original earlier ITERN timeout and
  admission proof remain in the immutable proof package.
- V1 public C lua_next still hits SIGALRM at four seconds. Its all-thread GDB
  capture independently shows api_stackroot_capture_edge -> lj_tab_wait_l at
  lj_api.c:315, before the iterator helper, with the other thread still paused
  in real gc2_idle_reclaim_enter. This is an unresolved runtime progress
  limitation, not a skipped test or accepted completion. lj_api.c is unchanged.
- Fixture development produced 18 failed runtime processes, 5 superseded
  successful processes and 2 failed compiles. All original fixture generations
  and outputs remain present; fixture-generations.md explains each correction.
  They are excluded from the 123 final successes. The GDB interruption is an
  observation, never a runtime pass.

Final proposed fixture entrypoints are t-tab-scalar-next-progress.c;
t-tab-scalar-next-authority-v6.c; t-tab-scalar-next-stack-retry.c; and
t-tab-scalar-next-lifetime.c, which includes the exact v6 authority file.
The latter three require TAB/GC2 helpers (authority/lifetime also ARENA). The
optimized GC2-only archive intentionally does not export the direct probe;
its production path is covered by the 24 progress cases and unchanged idle
regression. Exact commands, dependency/source/archive/binary hashes and timeout
limits are retained in build and validation JSON files.

No source changes, source rebuilds, tests, or documentation writes occurred in
the shared workspace. No commits were made. ROOT owns canonical registration,
an explicitly identified future combination, and the separate receiver-capture
proposal review. General hash/GC-output/arena-writer progress remains open.
