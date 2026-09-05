# Function-root publication under additional metamethod leases

Read-only design audit, 2026-09-05. This covers the proposed first function-valued
cdata method capture, not an implementation or a new test result. Source versions
are in `publication-source-manifest.json`; the frozen normal candidate is the
reference for these line numbers.

The extra metatable and method leases can survive method-root publication under
the current internal allocator policy. They should be released immediately after
that barrier, before the unchanged receiver/key barriers. Copy all three anchor
words before leaving source SMR; publish the method anchor while all exact scopes
remain held. Once published, the enumerated private method anchor supplies the
authority used by the existing method-frame handoff.

## Nonthrowing function publication route

| Source | Relevant behavior |
| --- | --- |
| lj_gc.c:4400, lj_gc2.c:15486 | `lj_gc_pubroot` dispatches to the GC2 TValue barrier. Non-generational IDLE returns; MARK/WEAK marks/rescans; SWEEP retains and publishes the semantic edge; generational IDLE remembers it. |
| lj_gc2.c:16473, :7330, :7287 | Function direct-body preservation handles the Lua proto/upvalue bodies without calling Lua. A function rescan publishes a queue identity; it does not run the function or traverse its arbitrary child graph synchronously. C functions bypass Lua-proto preservation. |
| lj_gc2.c:14536, :14444, :14369 | Mutator publication appends to the current SSB. A full buffer tries a free SSB node and may recycle a published node. Failure falls back to the allocation-side recovery plane. |
| lj_gc2.c:14344, :14824, :14643 | Writer-side recycling is MARK/WEAK only, owns `worker_active`, and converts at most one SSB-node budget to grey/recovery. It preserves and schedules individual identities; it does not drain arbitrary grey work or dispatch finalizers. A failed conversion retains its exact source slot and count. |
| lj_gc2.c:14635, :10968, :10914 | Grey push can grow storage. `gc2_grey_grow` calls `lj_mem_new_nothrow`; NULL returns failure without replacing the old queue. The publisher then tries recovery, or sets the reclaim veto. |
| lj_gc.c:4700, :4580 | The nonthrowing allocator selects the existing current-TG arena descriptor, returns NULL before accounting on failure, and never translates that NULL to `lj_err_mem`. It does not allocate a TG or a Lua frame to find the descriptor. |
| lj_arena.c:8759, :8996 | Internal new-storage allocation uses arena allocation or a huge mapping plus registry insertion. Failed allocation/insertion returns NULL; no `lj_err`, longjmp, Lua-call or safepoint-wait site exists in this implementation. Ordinary assertions/abort remain fatal invariant checks. |
| lj_gc.c:5646 | Successful queue replacement frees the old plain storage through the same internal allocator. This does not invoke a Lua destructor/finalizer. |
| lj_gc2.c:3237, :3249, :14975, :1494 | Successful queue growth can cross allocation accounting's checkpoint. Its nested assist must first acquire `worker_active`; the SSB converter already owns that token, so the nested attempt returns before arbitrary mark/weak work. |
| lj_gc2.c:10545, :10637, :824 | Queue failure falls back to existing small/Huge recovery metadata, or installs sticky `recovery_failed`/NO_RECLAIM and wakes a worker. These paths do not allocate or throw. The fault-only activation CAS retry loop is not a fixed-step progress guarantee. |
| lj_gc2.c:15247, :2892 | Generational-IDLE remembering disables writer-side queue draining. Overflow may request a cycle by atomics/threshold publication and worker wake; it does not synchronously run a collector or callback. |
| lj_gc2.c:22010 | Public SWEEP edges preserve the admitted body and schedule graph-bearing objects through the mutator queue. Strings/cdata are graphless; no FFI method is invoked by preserving them. |

The current policy is explicit in lj_state.h:183:
`LJ_GC2_INTERNAL_ALLOCATOR_ONLY` defaults to 1. Under this gate lua_newstate
(lj_state.c:1220) replaces the supplied callback with the internal arena allocator,
and lua_setallocf (lj_api.c:3067) is a no-op. With the gate disabled,
gc_mem_new_nothrow has a direct arbitrary callback branch. This audit makes no
nonthrowing claim for that branch. Compile the optional cdata attempt to refusal
unless both LJ_HASFFI and LJ_GC2_INTERNAL_ALLOCATOR_ONLY are enabled; the existing
lookup remains available.

## Why method publication comes first

The candidate key is generic. In particular, publishing an already-marked
table-valued key can take gc2_root_rescan_later's immediate gc2_traverse_tab fallback
after failed rescan publication (lj_gc2.c:7287). That path is much broader than the
function rescan, including child scheduling and weak-table bookkeeping. A full
exception and worker-ownership audit of that preexisting fallback is outside this
bounded change. Adding two more live scopes across it is unnecessary.

Publishing the method and releasing the extra scopes first preserves all existing
input-root semantics and leaves the table-key path with exactly its existing
input leases. This does not claim to repair or certify that broader legacy path.

## Cleanup and retained negative capability

- No optional target lease is acquired before all chain anchors exist and the
  original receiver/key have their exact admissions. Anchor OOM therefore stays
  on the existing explicit unwind path before admission.
- `lj_gc2_tv_lease_acquire` initializes its output and releases any internal scope
  on a non-VALID result (lj_gc2.c:16589). An optional failed method admission owns
  no method token; release the successfully acquired metatable token before
  falling back. Neither absent nor nonfunction results publish the method anchor.
- On optional success, publish the exact function anchor, then unconditionally
  release method and metatable. An allocator NULL does not escape this tail.
  Publish/release original receiver/key through the unchanged input path. Return
  a first-method-ready flag only after every scope closes.
- Keep `meta_test_pause_after_mt_capture` immediately after the fresh base-root
  load and `meta_test_pause_after_mt_lease` after successful exact target admission.
  The first boundary tests source-SMR protection before target admission; the
  second tests the retained incarnation after replacement. Test-only spinning is
  an intentional schedule hook, not production behavior.
- Preserve one-shot `lj_tab_getstr_held_try`, including generation, retiring,
  key-lock, forwarding and publication-claim refusal. Do not replace it with
  `lj_tab_getstr`, `lj_tab_gettv_forjit` or any helper that can wait/reenter.
- Forced admission RETRY, method replacement, base-metatable replacement and
  caught Lua/C/FFI callback errors must cover the new optional path. Frame setup,
  table waits and callbacks execute only after the complete scope cleanup.

## Evidence limit

This is source reasoning. The previous rooted-positive-hit pressure probe at
`/tmp/lj-rooted-hit-queue-growth-20260905-nhG4iM9e` exercised real full-SSB conversion
and successful grey growth; it did not exercise allocator failure. The existing
`lj_gc2_test_recovery_fail_grey_grow` hook returns failure before allocation and can
prove the queue-failure/recovery cleanup branch, but cannot be described as a real
OS/allocator OOM. The proposed extra-lease path still requires its own functional
evidence. No benchmark, production edit or test was performed by this audit.
